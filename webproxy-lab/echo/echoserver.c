#include "csapp.h" // CS:APP 공용 헤더: 안전 래퍼(대문자 버전)와 상수(MAXLINE) 선언
#include "echo.c"

void echo(int connfd); // 한 연결을 처리(클라이언트가 보낸 줄을 그대로 돌려보냄)하는 함수 원형

int main(int argc, char **argv) {
    int listenfd, connfd;               // listenfd: 리스닝 소켓 FD(문지기), connfd: 손님 전용 새 소켓 FD
    socklen_t clientlen;                // accept에 넘길 "주소 버퍼 길이" (입력으로 주고, 실제 길이로 갱신됨)
    struct sockaddr_storage clientaddr; // 어느 주소 체계(IPv4/IPv6)든 수용 가능한 "넉넉한" 주소 버퍼
    char client_hostname[MAXLINE],      // getnameinfo로 뽑아낼 "상대 호스트 문자열"
        client_port[MAXLINE];           // getnameinfo로 뽑아낼 "상대 포트 문자열"

    if (argc != 2) { // 인자 검증: ./echoserveri <port>
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(0); // 책 관례대로 0으로 종료(실전이라면 비정상 코드를 쓰기도 함)
    }

    listenfd = Open_listenfd(argv[1]); // 헬퍼: 지정 포트로 리스닝 소켓 생성
                                       // 내부: getaddrinfo(NULL, port, AI_PASSIVE) → socket → SO_REUSEADDR →
                                       //       bind → listen(backlog). 실패 시 메시지 출력 후 종료(대문자 버전)

    while (1) {                                         // 반복 서버: 한 번에 한 손님씩 순차 처리
        clientlen = sizeof(struct sockaddr_storage);    // 커널에 주소를 써줄 "버퍼 크기" 알려주기
        connfd = Accept(listenfd,                       // 커널의 완료 큐에서 한 연결을 꺼냄
                        (SA *)&clientaddr, &clientlen); // SA는 보통 'struct sockaddr'의 typedef/매크로
                                                        // 성공 시: 그 손님만 위한 새 FD(connfd) 반환

        Getnameinfo((SA *)&clientaddr, clientlen, // 이진 주소 → 사람이 읽을 표기로 변환
                    client_hostname, MAXLINE,     // 예: "203.0.113.10"
                    client_port, MAXLINE,         // 예: "53124"
                    0); // 플래그 0: 필요하면 역방향 이름 조회 가능(환경에 따라 느려질 수 있음)

        printf("Connected to (%s, %s)\n", // 접속한 클라이언트 표기
               client_hostname, client_port);

        echo(connfd); // 핵심 처리: 이 연결에서 들어오는 줄을 그대로 돌려보냄
                      // (보통 RIO로 readline → writen 반복, EOF까지)

        Close(connfd); // 해당 손님과의 전용 소켓 닫기 (리스닝 소켓은 유지)
    }

    exit(0); // (이 루틴은 통상 무한 루프라 도달하지 않음)
}