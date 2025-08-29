#include "csapp.h" // RIO와 각종 안전 래퍼(대문자) 선언, 상수 MAXLINE 등 포함

void echo(int connfd) // 클라이언트 1명과의 통신을 담당하는 함수(연결 전용 FD를 인자로 받음)
{
    size_t n;          // 읽은 바이트 수를 저장.
    char buf[MAXLINE]; // 한 줄을 담을 버퍼 (보통 8192바이트)
    rio_t rio;         // RIO의 내부 상태/버퍼(이 FD에 바인딩됨)

    Rio_readinitb(&rio, connfd); // RIO 초기화: connfd에 대해 내부 버퍼를 준비
                                 // 이후 rio_readlineb는 이 버퍼를 사용해 short read/EINTR를 자동 보정

    while ((n = Rio_readlineb(&rio, buf, MAXLINE)) != 0) {
        // 클라이언트가 보낸 "한 줄"을 안전하게 읽음
        // 반환값 n: '\n' 포함 실제로 읽은 바이트 수 (EOF면 0)
        printf("server received %d bytes\n", (int)n);
        // 디버그 출력: 이번에 받은 바이트 수를 서버 콘솔에 알림
        // (connfd로 보내는 게 아니라 서버 표준출력으로 찍음)

        Rio_writen(connfd, buf, n); // 방금 받은 그대로 클라이언트에게 다시 전송(에코)
                                    // write의 부분 쓰기/시그널을 내부에서 반복해 "정확히 n바이트" 보장
    }
}