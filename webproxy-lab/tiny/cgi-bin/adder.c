/*
 * adder.c - a minimal CGI program that adds two numbers together
 */
/* $begin adder */
#include "csapp.h"

int main(void) {
    char *buf, *p; // buf : 쿼리 문자열을 가리키는 포인터, p : & 위치를 가리킬 포인터
    char arg1[MAXLINE], arg2[MAXLINE], content[MAXLINE]; // 두 피연산자 보관 버퍼, 최종 HTML 컨텐츠 버퍼
    int n1 = 0, n2 = 0;                                  // 정수로 변환된 두 값 (0으로 초기화)

    /* Extract the two arguments */               // 쿼리 문자열에서 두 숫자를 분리해 추출
    if ((buf = getenv("QUERY_STRING")) != NULL) { // 웹서버가 넘긴 환경변수 QUERY_STRING 가져오기 (ex: 123&45)
        p = strchr(buf, '&');                     // & 구분자의 위치 찾기(좌 : 첫 번째 수, 우 : 두 번째 수)
        *p = '\0';           // &를 문자열 끝으로 바꿔 왼쪽을 독립 문자열로 분리. 왼쪽과 오른쪽이 나눠지게됨
        strcpy(arg1, buf);   // 왼쪽만 arg1에 복사 (123)
        strcpy(arg2, p + 1); // 오른쪽을 arg2에 복사 (45)
        /**
         * 책에서는 쿼리스트링이 123&45로 작성됨을 가정하였지만 이 코드에서는 x=123&y=45와 같은 쿼리스트링을 받는
         * 방식으로 처리함 strchr(arg1, '=')는 =가 가리키는 포인터를 반환하고, 여기에 1을 더해주면
         * = 바로 옆의 숫자 시작 문자(1)을 가리킬 수 있게 됨
         * 그 후 atoi를 통해 숫자 문자열을 정수로 변환하여 n1에 저장함, 결과적으로 원하는 숫자만 추출하는 단계가 하나 더
         * 추가된 것
         */
        n1 = atoi(strchr(arg1, '=') + 1);
        n2 = atoi(strchr(arg2, '=') + 1);
    }

    /* Make the response body */                                // 클라이언트에 보낼 HTML 바디(문자열) 만들기
    sprintf(content, "QUERY_STRING=%s\r\n<p>", buf);            // content에 쓸 첫 문장 기록
    sprintf(content + strlen(content), "Welcome to add.com: "); // 바로 덮어씀. 안내 문구로 content 초기화
    sprintf(content + strlen(content),
            "THE Internet addition portal.\r\n<p>"); // 기존 content 뒤에 문장을 계속 누적하여 이어붙임
    sprintf(content + strlen(content), "The answer is: %d + %d = %d\r\n<p>", n1, n2, n1 + n2); // 계산 결과를 이어붙임
    sprintf(content + strlen(content), "Thanks for visiting!\r\n");                            // 마지막 인사말 이어붙임

    /* Generate the HTTP response */
    /**
     * 책의 예제와 또 다름. 하지만 HTTP/1.x에서 헤더의 순서는 중요하지 않음
     * 그래서 Content-Type -> Content-length(사용자 코드)든
     * Content-length->Content-type이든 동일하게 해석됨
     */
    printf("Content-type: text/html\r\n");                  // MIME 타입이 text/html임을 나타냄
    printf("Content-length: %d\r\n", (int)strlen(content)); // 바디의 바이트 수 - 정확해야 브라우저가 끊김없이 읽음
    printf("\r\n");                                         // 마지막 빈 줄료 헤더 종료 신호
    printf("%s", content);                                  // 아까 만든 HTML 바디 전송
    fflush(stdout);                                         // 표준출력 버퍼를 즉시 비워 네트워크로 흘러가게 함

    exit(0); // 정상 종료. 웹 서버는 이 프로세스의 종료를 응답 끝으로 인식함
}
/* $end adder */
