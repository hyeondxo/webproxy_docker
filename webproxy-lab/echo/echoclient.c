#include "csapp.h" // CS:APP 제공 헤더: RIO, 락퍼(대문자) 함수들, 상수(MAXLINE=보통 8192) 선언

int main(int argc, char **argv) // 프로그램 진입점: ./echoclient <host> <port> 형태로 실행됨
{
    int clientfd;                    // 서버에 연결된 "소켓 FD"를 저장할 변수
    char *host, *port, buf[MAXLINE]; // host/port는 명령행 인자 문자열을 가리킴, buf는 전송/수신용 라인 버퍼
    rio_t rio;                       // RIO의 내부 버퍼/상태를 담는 구조체 (해당 FD에 바인딩해 사용)

    if (argc != 3) { // 인자 개수 검증: 프로그램 이름 + host + port = 3 여야 함
        fprintf(stderr, "usage: %s <host> <port>\n", argv[0]);
        exit(0); // 사용법만 알려주고 종료 (에러 코드 0은 책의 관례)
    }
    host = argv[1]; // 첫 번째 인자를 호스트명/IP 문자열로 사용 (예: "localhost" 또는 "93.184.216.34")
    port = argv[2]; // 두 번째 인자를 서비스/포트 문자열로 사용 (예: "80", "15213")

    clientfd = Open_clientfd(host, port); // 헬퍼: getaddrinfo로 IPv4/IPv6 후보를 돌며 socket→connect 시도
                                          // 성공 시 "연결된 소켓 FD" 반환, 실패 시 메시지 출력 후 종료(대문자 래퍼)

    Rio_readinitb(&rio, clientfd); // RIO 초기화: clientfd와 rio 내부 버퍼를 연결
                                   // 이후 rio_readlineb/rio_readn 등은 이 버퍼를 통해 견고하게 I/O 수행

    while (Fgets(buf, MAXLINE, stdin) != NULL) { // 표준입력(키보드/파이프)에서 한 줄 읽기
                                                 // fgets의 대문자 래퍼: 실패 시 에러 처리 포함
        Rio_writen(clientfd, buf, strlen(buf));  // 서버로 방금 입력한 라인을 "정확히 n바이트" 다 보낼 때까지 반복 쓰기
                                                 // (부분 쓰기/시그널을 내부 루프로 보정)

        Rio_readlineb(&rio, buf, MAXLINE); // 서버가 돌려준 echo 응답을 "한 줄 단위"로 안전하게 읽기
                                           // 개행 문자까지 읽어오며, 반환된 문자열은 buf에 저장됨

        Fputs(buf, stdout); // 표준출력으로 그대로 내보내기(에코 출력)
                            // stdout이 터미널이면 보통 줄 버퍼링 → 개행과 함께 화면으로 즉시 출력
    }

    Close(clientfd); // 소켓 FD 닫기(대문자 래퍼: 에러 처리 포함). 서버 쪽은 EOF(0) 인지 가능
    exit(0);         // 정상 종료
}