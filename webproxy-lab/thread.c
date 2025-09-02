// Part II: 동시 처리 스레드 유닛
// - 이 파일은 proxy.c에서 텍스트로 포함(#include "thread.c")되어 같은 번역 단위로 컴파일
// - 설계: 연결당 스레드(thread-per-connection) + detach로 자원 누수 방지

typedef struct {
    int connfd; // 처리할 클라이언트 연결 소켓 FD
} thread_arg_t;

static void *proxy_thread_main(void *arg) {
    pthread_detach(pthread_self());        // 스레드를 즉시 detach하여 join 불필요
    thread_arg_t *a = (thread_arg_t *)arg; // 전달 인자 캐스팅
    int connfd = a->connfd;                // FD 로컬 복사
    free(a);                               // 인자 구조체 해제
    handle_client(connfd);                 // 요청 처리
    close(connfd);                         // 연결 종료
    return NULL;                           // 반환값 없음
}

static int spawn_detached_worker(int connfd) {
    pthread_t tid;                                                  // 새 스레드 ID
    thread_arg_t *a = (thread_arg_t *)malloc(sizeof(thread_arg_t)); // 인자 동적 할당
    if (!a) {                                                       // 메모리 부족
        close(connfd);                                              // FD 닫기
        return -1;                                                  // 에러
    }
    a->connfd = connfd;                                        // FD 저장
    int rc = pthread_create(&tid, NULL, proxy_thread_main, a); // 스레드 생성
    if (rc != 0) {                                             // 실패 시 정리
        free(a);
        close(connfd);
        errno = rc;
        return -1;
    }
    return 0; // 성공
}
