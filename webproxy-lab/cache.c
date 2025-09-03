#include "cache.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

// 캐시 엔트리 구조체
typedef struct cache_entry {
    char *key;                // 식별자(URI)
    char *data;               // 응답 원본 데이터(헤더 포함)
    size_t size;              // 데이터 바이트 수
    struct cache_entry *prev; // LRU 리스트 이전 노드
    struct cache_entry *next; // LRU 리스트 다음 노드
} cache_entry_t;

// 전역 캐시 상태
static cache_entry_t *head = NULL; // LRU의 처음 : 가장 최근 사용 캐시
static cache_entry_t *tail = NULL; // LRU의 마지막 : 가장 오래된 캐시
static size_t current_size = 0;    // 현재 저장된 캐시 크기의 총 합

// 타입 : pthread_rwlock_t는 POSIX 스레드의 읽기-쓰기 락 타입
// 여러 스레드가 동시에 읽기는 가능하고, 쓰기는 하나의 스레드만 단독으로 들어갈 수 있게 보장함.
// 캐시 조회는 빈번하고 읽기 비중이 높기 때문에 mutex 대신 RWLock을 쓰면 성능상 유리(동시 읽기 병행)
static pthread_rwlock_t cache_lock = PTHREAD_RWLOCK_INITIALIZER;

// LRU 리스트 앞에 삽입
static void insert_head(cache_entry_t *entry) {
    entry->prev = NULL;     // head가 되면 prev가 null임
    entry->next = head;     // next를 기존의 head로 -> 맨 앞이됨
    if (head)               // 기존 리스트가 비어있지 않았다면
        head->prev = entry; // 기존 head의 이전 노드로 entry가 됨
    else                    // 비어있었다면
        tail = entry;       // 첫 노드로써 추가되므로 tail도 entry가 됨
    // LRU 리스트의 head는 새로 삽입한 entry가 됨
    head = entry;
}

// LRU 리스트에서 제거
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

// LRU 리스트에서 가장 오래된 항목 제거(필요 시 반복)
static void remove_tail(size_t need) {
    // 1. 현재 크기 + 필요 크기 > 최대 크기 이고,
    // 2. 제거할 대상이 있을 때(tail 존재) 반복
    while ((current_size + need > MAX_CACHE_SIZE) && tail) {
        // 가장 마지막 캐시 데이터부터 제거
        cache_entry_t *entry = tail;
        list_remove(entry);
        current_size -= entry->size; // 현재 크기에서 제거될 엔트리 크기만큼 차감
        free(entry->key);            // key 해제
        free(entry->data);           // 데이터 해제
        free(entry);                 // 엔트리 구조체 자체를 해제
    }
}

// 프로세스 시작 시 캐시 전역 상태를 깨끗한 초기 상태로 만든다
void cache_init(void) {
    // rw락을 기본 속성으로 초기화
    pthread_rwlock_init(&cache_lock, NULL);
    head = tail = NULL; // LRU 이중 연결 리스트 초기화
    current_size = 0;   // 캐시에 저장된 객체 바이트 합계 초기화
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
    current_size = 0;
    // 파괴 직전 잠금해제
    pthread_rwlock_unlock(&cache_lock);
    // 락 자체를 파괴. 사용불가 상태가 됨
    pthread_rwlock_destroy(&cache_lock);
}

// LRU 리스트를 head부터 순회하며 key를 통해 캐시 엔트리를 찾는다
static cache_entry_t *find_cache(const char *key) {
    for (cache_entry_t *entry = head; entry; entry = entry->next) {
        if (strcmp(entry->key, key) == 0)
            return entry;
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
    cache_entry_t *entry = find_cache(key);
    if (!entry) { // MISS라면 락을 풀고 0 반환
        pthread_rwlock_unlock(&cache_lock);
        return 0; // MISS
    }
    // 엔트리 있으면 복사 시도
    char *copy = (char *)malloc(entry->size);
    if (!copy) {
        pthread_rwlock_unlock(&cache_lock);
        return -1; // OOM
    }
    memcpy(copy, entry->data, entry->size); // 캐시된 바이트 그대로 복사
    size_t sz = entry->size;
    pthread_rwlock_unlock(&cache_lock); // 락을 풀어 다른 rw가 접근 가능

    // LRU 갱신: 짧은 구간만 쓰기 락으로 잡고 리스트 앞으로 이동
    if (pthread_rwlock_wrlock(&cache_lock) == 0) {
        // 방금 쓴 캐시를 찾아서 "최신 사용"으로 갱신하기
        cache_entry_t *used_entry = find_cache(key);
        if (used_entry) {
            list_remove(used_entry); // 잠깐 지우고
            insert_head(used_entry); // 다시 앞에 넣는다
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
    cache_entry_t *entry = (cache_entry_t *)malloc(sizeof(cache_entry_t));
    if (!entry)
        return;
    // 키 문자열을 복사하고 데이터 저장 공간을 size만큼 확보
    char *k = strdup(key);
    char *d = (char *)malloc(size);
    if (!k || !d) {
        free(entry);
        free(k);
        free(d);
        return;
    }
    // 원본 바이트를 내부 버퍼 d로 그대로 복사
    memcpy(d, data, size);
    // 엔트리 필드 초기화
    entry->key = k;
    entry->data = d;
    entry->size = size;
    entry->prev = entry->next = NULL; // 아직 연결 전이므로 NULL
    // 쓰기 락 획득. 삽입이나 교체는 모드 write-critical 영역이기 때문
    pthread_rwlock_wrlock(&cache_lock);
    // 동일 키가 이미 존재하면 제거(간단 일관성 유지)
    cache_entry_t *old = find_cache(key);
    if (old) {
        // 제거 후 메모리도 해제
        list_remove(old);
        current_size -= old->size;
        free(old->key);
        free(old->data);
        free(old);
    }

    // 캐시 공간 확보 후 삽입할 때
    // 새 엔트리 넣기 전 여유 공간 확보
    remove_tail(size);
    // 맨 앞으로 삽입
    insert_head(entry);
    // 캐시 총 크기 갱신
    current_size += size;
    pthread_rwlock_unlock(&cache_lock); // 쓰기 락 해제 -> 다른 스레드의 읽기 + 쓰기 허용
}
