#include "cache.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// 캐시 엔트리 구조: 키/데이터/크기 + LRU 메타데이터(이중연결리스트)
typedef struct cache_entry {
    char *key;                // 식별자(정규화된 URI 등)
    char *data;               // 응답 원본 바이트(헤더 포함)
    size_t size;              // 데이터 바이트 수
    struct cache_entry *prev; // LRU 리스트 이전 노드
    struct cache_entry *next; // LRU 리스트 다음 노드
} cache_entry_t;

// 전역 캐시 상태
// 타입 : pthread_rwlock_t는 POSIX 스레드의 읽기-쓰기 락 타입
// 여러 스레드가 동시에 읽기는 가능하고, 쓰기는 하나의 스레드만 단독으로 들어갈 수 있게 보장함.
// 캐시 조회는 빈번하고 읽기 비중이 높기 때문에 mutex 대신 RWLock을 쓰면 성능상 유리(동시 읽기 병행)
static pthread_rwlock_t cache_lock = PTHREAD_RWLOCK_INITIALIZER;
static cache_entry_t *head = NULL; // LRU의 가장 최근 사용(MRU) 앞쪽
static cache_entry_t *tail = NULL; // LRU의 가장 오래된(LRU) 뒤쪽
static size_t current_bytes = 0;   // 현재 저장된 총 바이트 수(객체 바이트 합계)

// 내부 유틸: 리스트 앞에 삽입
static void list_push_front(cache_entry_t *e) {
    e->prev = NULL;     // 새로 맨 앞에 올 노드는 prev가 없음
    e->next = head;     // next를 head : e가 맨 앞이됨
    if (head)           // 기존 리스트가 비어있지 않았다면
        head->prev = e; // head의 이전은 e. 양방향 연결
    else                // 비어있었다면
        tail = e;       // 첫 노드가 추가되므로 head와 tail은 모두 e임
    head = e;
}

// 내부 유틸: 리스트에서 제거(연결 해제)
static void list_remove(cache_entry_t *e) {
    if (e->prev)                 // 제거 대상 이전이 있다면
        e->prev->next = e->next; // e 이전을 다음으로 바로 연결 (자신이 빠짐)
    else                         // 이전 없었으면
        head = e->next;          // head였단 의미이므로 다음을 head로 갱신
    if (e->next)                 // 다음이 있었다면
        e->next->prev = e->prev; // 다음의 이전을 본인이 아닌 prev로 변경 (자신이 빠짐)
    else                         // 다음이 없었다면 -> tail이란 뜻
        tail = e->prev;          // tail로 이전을 연결
    e->prev = e->next = NULL;    // 자신의 양방향 포인터 초기화
}

// 내부 유틸: 가장 오래된 항목 제거(필요 시 반복)
static void evict_until_fit(size_t need) {
    // 필요한 용량과 현재 용량이 캐시 사이즈보다 크고(줄여야 함), 제거할 대상이 있을 때(tail 존재) 반복
    while (current_bytes + need > MAX_CACHE_SIZE && tail) {
        // 가장 마지막 캐시 데이터부터 제거
        cache_entry_t *e = tail;
        list_remove(e);
        // 실제 객체 바이트 총합에서 e 크기만큼 차감
        current_bytes -= e->size;
        free(e->key);  // 원본 문자열 해제
        free(e->data); // 바이트 해제
        free(e);       // 엔트리 구조체 자체를 해제
    }
}

// 프로세스 시작 시 캐시 전역 상태를 깨끗한 초기 상태로 만든다
void cache_init(void) {
    // rw락을 기본 속성으로 초기화
    pthread_rwlock_init(&cache_lock, NULL);
    head = tail = NULL; // LRU 이중 연결 리스트 초기화
    current_bytes = 0;  // 캐시에 저장된 객체 바이트 합계 초기화
}

// 종료 시 캐시에 남은 모든 엔트리를 해제하고 동기화 자원을 파괴
void cache_destroy(void) {
    // 쓰기 락으로 단독 진입, 다른 스레드가 캐시를 만지지 못하도록 막음(파괴 중 경쟁 방지)
    pthread_rwlock_wrlock(&cache_lock);
    // LRU 리스트를 순회하며 모든 엔트리를 해제
    for (cache_entry_t *p = head; p;) {
        cache_entry_t *nxt = p->next;
        free(p->key);
        free(p->data);
        free(p);
        p = nxt;
    }
    // 리스트 비우기
    head = tail = NULL;
    current_bytes = 0;
    // 파괴 직전 잠금해제
    pthread_rwlock_unlock(&cache_lock);
    // 락 자체를 파괴. 사용불가 상태가 됨
    pthread_rwlock_destroy(&cache_lock);
}

// 선형 탐색으로 간단히 구현(엔트리 수가 많지 않음을 전제). 필요 시 해시로 개선 가능.
static cache_entry_t *find_unlocked(const char *key) {
    for (cache_entry_t *p = head; p; p = p->next) {
        if (strcmp(p->key, key) == 0)
            return p;
    }
    return NULL;
}

// 캐시에 key로 저장된 웹 객체가 있는지 조회
// -> 있으면 그 바이트 데이터를 새로 할당한 복사본으로 되돌려줌
// key : 정규화된 URI 식별자
// data_out : HIT 시, 캐시된 객체 바이트를 새로 malloc한 포인터
// size_out : 해당 바이트 길이
// 반환값 : 1(HIT), 0(MISS), 음수(에러)
int cache_get(const char *key, char **data_out, size_t *size_out) {
    if (!key || !data_out || !size_out)
        return -1;
    *data_out = NULL;
    *size_out = 0;

    // 읽기 락으로 탐색하고 데이터 복사본을 만든다(다중 리더 동시 허용)
    if (pthread_rwlock_rdlock(&cache_lock) != 0)
        return -1;
    // 키가 있는지 탐색
    cache_entry_t *e = find_unlocked(key);
    if (!e) { // MISS라면 락을 풀고 0 반환
        pthread_rwlock_unlock(&cache_lock);
        return 0; // MISS
    }
    // 엔트리 있으면 복사 시도
    char *copy = (char *)malloc(e->size);
    if (!copy) {
        pthread_rwlock_unlock(&cache_lock);
        return -1; // OOM
    }
    memcpy(copy, e->data, e->size); // 캐시된 바이트 그대로 복사
    size_t sz = e->size;
    pthread_rwlock_unlock(&cache_lock); // 락을 풀어 다른 rw가 접근 가능

    // LRU 근사 갱신: 짧은 구간만 쓰기 락으로 잡고 리스트 앞으로 이동
    if (pthread_rwlock_wrlock(&cache_lock) == 0) {
        // 존재 확인 후 앞으로 이동
        cache_entry_t *e2 = find_unlocked(key);
        if (e2) {                // 최근 사용으로 간주되도록 리스트에서 맨 앞으로 이동
            list_remove(e2);     // 지우고
            list_push_front(e2); // 다시 앞에 넣는다
        }
        pthread_rwlock_unlock(&cache_lock);
    }

    *data_out = copy;
    *size_out = sz;
    return 1; // 성공적으로 복사했으면 1(HIT)반환
}

// 크기가 알맞다면 캐시에 저장.
// 같은 키가 이미 있으면 교체하고 공간이 모자라면 LRU로 공간 확보 후 삽입
void cache_put(const char *key, const char *data, size_t size) {
    if (!key || !data)
        return;
    if (size == 0 || size > MAX_OBJECT_SIZE)
        return; // 정책상 큰 객체는 캐시하지 않음

    // 새 캐시 엔트리 구조체 동적 할당
    cache_entry_t *e = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (!e)
        return;
    // 키 문자열을 복사하고 데이터 저장 공간을 size만큼 확보
    char *k = strdup(key);
    char *d = (char *)malloc(size);
    if (!k || !d) {
        free(e);
        free(k);
        free(d);
        return;
    }
    // 원본 바이트를 내부 버퍼 d로 그대로 복사
    memcpy(d, data, size);
    // 엔트리 필드 초기화
    e->key = k;
    e->data = d;
    e->size = size;
    e->prev = e->next = NULL; // 아직 연결 전이므로 NULL
    // 쓰기 락 획득. 삽입이나 교체는 모드 write-critical 영역이기 때문
    pthread_rwlock_wrlock(&cache_lock);
    // 동일 키가 이미 존재하면 제거(간단 일관성 유지)
    cache_entry_t *old = find_unlocked(key);
    if (old) {
        // 제거 후 메모리도 해제
        list_remove(old);
        current_bytes -= old->size;
        free(old->key);
        free(old->data);
        free(old);
    }
    // 공간 확보 후 삽입
    evict_until_fit(size);              // 새 엔트리 넣기 전 여유 공간 확보
    list_push_front(e);                 // 맨 앞으로 삽입
    current_bytes += size;              // 캐시 총 크기 갱신
    pthread_rwlock_unlock(&cache_lock); // 쓰기 락 해제 -> 다른 스레드의 읽기 + 쓰기 허용
}
