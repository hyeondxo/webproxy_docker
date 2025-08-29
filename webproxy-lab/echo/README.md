# Echo 서버/클라이언트 실습 가이드

간단한 TCP Echo 서버/클라이언트를 통해 소켓 프로그래밍과 CS:APP의 Robust I/O(RIO) 래퍼 사용법을 실습합니다. 서버는 클라이언트가 보낸 한 줄을 그대로 돌려주는 반복 서버입니다.

## 구성 파일
- `echoserver.c`: 단일 프로세스 반복 서버. 접속을 받아 `echo()`로 처리.
- `echo.c`: 한 연결에서 라인 단위로 읽고 그대로 다시 쓰는 `echo()` 구현.
- `echoclient.c`: 표준입력으로부터 읽어 서버에 보내고, 응답을 그대로 출력하는 클라이언트.
- `csapp.c`, `csapp.h`: CS:APP 제공 안전 래퍼와 RIO 구현.
- `Makefile`: 서버/클라이언트 빌드 스크립트.

## 빌드
`webproxy-lab/echo` 디렉터리에서 다음 명령으로 각각 빌드하세요.

```bash
# 서버 바이너리 빌드
make echoserver

# 클라이언트 바이너리 빌드
make echoclient

# 정리
make clean
```

빌드가 완료되면 현재 디렉터리에 `echo_server`, `echo_client` 실행 파일이 생성됩니다.

> 참고: `Makefile`의 기본 `all` 타깃은 환경에 따라 동작이 다를 수 있으므로 위처럼 개별 타깃을 호출하는 것을 권장합니다.

## 실행 방법

### 1) 서버 실행
원하는 포트 번호(1024 이상, 사용 중이지 않은 포트)를 지정합니다.

```bash
./echo_server <port>
# 예) 15213 포트로 서버 시작
./echo_server 15213
```

서버 로그 예시
```
Connected to (127.0.0.1, 54321)
server received 6 bytes
```

### 2) 클라이언트 실행
서버가 동작 중인 호스트와 포트를 지정합니다.

```bash
./echo_client <host> <port>
# 예) 같은 머신에서 실행 중인 서버에 접속
./echo_client localhost 15213
```

이후 키보드로 입력한 각 줄이 서버를 거쳐 그대로 출력됩니다.

### 3) `nc`/`telnet`으로 빠른 테스트(선택)
클라이언트 대신 `nc`(netcat)나 `telnet`으로도 서버를 시험할 수 있습니다.

```bash
# netcat
nc localhost 15213
hello
hello

# telnet
telnet localhost 15213
world
world
```

## 동작 예시
두 개의 터미널을 사용합니다.

- 터미널 A (서버)
```bash
./echo_server 15213
Connected to (127.0.0.1, 57422)
server received 6 bytes
server received 4 bytes
```

- 터미널 B (클라이언트)
```bash
./echo_client localhost 15213
hello\n   # 입력
hello     # 서버가 돌려준 응답
bye\n
bye
```

또는 파이프/리다이렉션으로도 확인할 수 있습니다.
```bash
echo "foo" | ./echo_client localhost 15213
```

## 코드 훑어보기
- `echo.c`의 `echo(int connfd)`: `rio_readlineb`로 한 줄씩 읽고, `Rio_writen`으로 그대로 반환합니다. 서버 콘솔에는 받은 바이트 수를 출력합니다.
- `echoserver.c`: `Open_listenfd(port)`로 리스닝 소켓을 만들고, `Accept`로 연결을 받아 `echo(connfd)` 호출 후 `Close`합니다. 간단한 반복 서버 구조입니다.
- `echoclient.c`: `Open_clientfd(host, port)`로 서버에 연결하고, 표준입력에서 읽은 라인을 `Rio_writen`으로 보내고 `Rio_readlineb`로 응답을 받아 출력합니다.

RIO 함수들은 부분 읽기/쓰기, 시그널 인터럽트 등을 내부에서 보정하여 안전한 I/O를 제공합니다.

## 트러블슈팅
- 포트 사용 중 오류: 같은 포트를 쓰는 프로세스가 있으면 실패합니다. 다른 포트를 사용하거나, 기존 프로세스를 종료하세요.
- 방화벽/컨테이너: 로컬이 아닌 원격 접속이 필요하면 방화벽 규칙과 컨테이너 포트 포워딩 설정을 확인하세요.
- IPv4/IPv6: 코드가 프로토콜 독립적으로 작성되어 있으며, `localhost` 또는 `127.0.0.1`로 접속이 되지 않을 때는 `::1`(IPv6 루프백)도 시도해 보세요.
- 줄 단위 프로토콜: `rio_readlineb`는 개행(`\n`)을 기준으로 라인을 완성하므로, 테스트 입력 끝에 개행을 포함해야 응답을 볼 수 있습니다.

## 정리
- 빌드: `make echoserver && make echoclient`
- 실행: `./echo_server <port>` → `./echo_client <host> <port>`
- 기대 동작: 입력한 라인이 그대로 되돌아옵니다.


