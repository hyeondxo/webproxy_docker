# Tiny 웹서버 실습 가이드
Chrome 브라우저와 시크릿 모드에서 원활한 실습이 가능함.
간단한 반복형 HTTP/1.0 서버(Tiny)를 통해 정적/동적 컨텐츠 제공, 소켓 프로그래밍, CGI 실행 흐름을 실습합니다. 서버는 한 번에 한 연결을 처리(동시성 없음)하며 GET 메서드만 지원합니다.

## 구성 파일
- `tiny.c`: 서버 메인 로직. 요청 파싱 → 정적(`serve_static`) 또는 동적(`serve_dynamic`) 처리.
- `csapp.c`, `csapp.h`: RIO(견고한 I/O)와 소켓/시스템 콜 래퍼.
- `cgi-bin/adder.c`: 예제 CGI 프로그램(동적 컨텐츠). `GET /cgi-bin/adder?x=1&y=2` 형태.
- `home.html`, `godzilla.jpg|gif`: 정적 파일 예제.
- `Makefile`: 빌드 스크립트(서버와 CGI 바이너리).

## 빌드
`webproxy-lab/tiny` 디렉터리에서 아래 중 하나를 실행하세요.

```bash
# 서버와 CGI 모두 빌드
make
# 또는
make all

# 서버만 빌드
make tiny

# CGI만 빌드
make cgi

# 정리
make clean
```

빌드 후 생성물
- 서버 실행 파일: `tiny`
- CGI 실행 파일: `cgi-bin/adder` (실행 권한 포함)

## 실행
원하는 포트(1024 이상, 비사용)로 서버를 실행합니다.

```bash
./tiny <port>
# 예) 8000 포트
./tiny 8000
```

서버 로그 예시
```
Accepted connection from (127.0.0.1, 53244)
Request headers:
GET /home.html HTTP/1.1
...
Response headers:
HTTP/1.0 200 OK
Server: Tiny Web Server
Connection: close
Content-length: 123
Content-type: text/html
```

## 정적 컨텐츠 테스트
브라우저 또는 `curl`로 접근합니다.

```bash
# 루트는 ./home.html로 매핑됨(디폴트 파일)
curl -v http://localhost:8000/

# HTML 파일
curl -v http://localhost:8000/home.html

# 이미지 파일
curl -v http://localhost:8000/godzilla.jpg --output /tmp/g.jpg
```

Tiny는 다음 순서로 정적 파일을 전송합니다.
1) MIME 결정(`get_filetype`) → 2) 헤더 작성/전송 → 3) `Open` → `Mmap` → `Rio_writen` → `Munmap`.

## 동적 컨텐츠(CGI) 테스트
`adder` CGI는 `x`와 `y`를 합산해 HTML로 응답합니다.

```bash
curl -v "http://localhost:8000/cgi-bin/adder?x=3&y=5"
```

동작 개요
- 서버가 `QUERY_STRING` 환경변수에 `x=3&y=5`를 설정 → `stdout`을 소켓으로 `Dup2` → `Execve("./cgi-bin/adder", ...)` 실행 → CGI의 표준출력이 그대로 클라이언트로 전송됩니다.

## 원시 요청으로 테스트(nc/telnet)
```bash
printf 'GET /home.html HTTP/1.0\r\n\r\n' | nc localhost 8000
```

HTTP/1.0이므로 요청 후 연결은 닫힙니다(`Connection: close`).

## Telnet 실습
Telnet으로 직접 HTTP 요청 라인을 타이핑하며 Tiny의 동작을 관찰합니다.

사전 준비: 서버 실행 중(`./tiny 8000`).

1) Telnet 접속
```bash
telnet localhost 8000
```

2) 정적 파일 요청(Enter를 두 번 눌러 빈 줄로 헤더 종료)
```
GET /home.html HTTP/1.0\r\n
```
화면에 HTML이 출력됩니다.

3) 이미지 요청(이진 데이터 주의)
```
GET /godzilla.jpg HTTP/1.0\r\n
```
터미널이 깨져 보일 수 있습니다. 저장하려면 telnet 대신 `nc`/`curl -o`를 사용하세요.

4) CGI 호출(동적 컨텐츠)
```
GET /cgi-bin/adder?x=3&y=5 HTTP/1.0\r\n
```
CGI가 출력한 헤더와 본문(“3 + 5 = 8”)이 표시됩니다.

팁
- HTTP/1.0에서는 요청라인 다음에 반드시 빈 줄이 필요합니다. 빈 줄을 보내지 않으면 응답이 오지 않습니다.
- HTTP/1.1로 시도하려면 `Host: localhost` 헤더를 추가하고 빈 줄을 보내세요.
HTTP/1.1로 보내는 법

필수: Host 헤더 포함 + 헤더 종료용 빈 줄

예시(텔넷에서 Enter로 줄바꿈, 마지막에 빈 줄):

GET /home.html HTTP/1.1
Host: localhost
Keep-Alive 원하면 명시:

GET /home.html HTTP/1.1
Host: localhost
Connection: keep-alive
즉시 닫고 싶으면:

GET /home.html HTTP/1.1
Host: localhost
Connection: close
Tip

curl로 확인: curl -v http://localhost:1234/home.html
HTTP/1.1에서도 Tiny는 요청 1개 처리 후 닫을 수 있으니, 연결 재사용은 서버 구현에 따라 달라집니다.
- IPv6 이슈가 있으면 `telnet 127.0.0.1 8000`로 접속하세요.

## 트러블슈팅
- 포트 충돌: 사용 중인 포트면 실패. 다른 포트를 사용하세요.
- 404/403: 요청 파일이 없거나 읽기 권한이 없으면 에러 응답(파일 유형/권한 검사: `S_ISREG`, `S_IRUSR`).
- 줄바꿈: Tiny는 헤더에 CRLF(`\r\n`)를 사용합니다. 원시 요청 시 빈 줄로 헤더 종료를 명확히 표기하세요.
- CGI 실행 권한: `cgi-bin/adder`에 실행 권한이 있어야 합니다. `make cgi`가 권한을 부여합니다.

## 설계 상 주의
- 단일 처리(반복 서버): 동시 접속은 직렬 처리됩니다.
- GET만 지원: 다른 메서드는 501 Not Implemented 응답.
- 보안: 학습용 서버로 입력 검증, 경로 탐색 방지, 환경변수 정리 등은 최소화되어 있습니다. 운영 환경에 사용하지 마세요.

## 유용한 테스트 팁
- 응답 헤더만 확인: `curl -I http://localhost:8000/home.html`
- 상세 로그: `curl -v ...`
- 이미지 확인: 받아서 뷰어로 열기 또는 크기만 확인 `file /tmp/g.jpg`.

## 정리
- 빌드: `make` (또는 `make tiny && make cgi`)
- 실행: `./tiny <port>`
- 정적: `/`, `/home.html`, `/godzilla.jpg|gif`
- 동적: `/cgi-bin/adder?x=3&y=5`

즐거운 실습 되세요!
