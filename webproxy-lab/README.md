Proxy Lab 실습 가이드 (Part I – 순차 프록시)

개요
- 목표: `webproxy-lab/proxy.c`에 구현한 HTTP/1.0 기반 GET 전용 프록시를 빌드하고 실습합니다.
- 범위: Part I(순차 처리)만 다룹니다. 헤더 재작성(Host/User-Agent/Connection/Proxy-Connection)과 바이너리 안전 스트리밍을 수행합니다.

사전 준비물
- 컴파일러: gcc 또는 clang
- 유틸리티: curl, nc(netcat), 쉘 환경
- 참고: 아래 명령은 저장소 루트에서 실행한다고 가정합니다. 경로가 다르면 적절히 수정하세요.

빌드
- 프로젝트 루트(= `webproxy-lab` 디렉터리)에서 한 번에 빌드/정리:
  - `make clean && make all`
- 생성물:
  - 프록시 실행 파일: `proxy`
  - Tiny 서버 실행 파일: `tinyserver`
- 수동 빌드(대안):
  - 프록시: `gcc -Wall -Wextra -O2 -I tiny -o proxy proxy.c tiny/csapp.c`
  - Tiny: `gcc -Wall -Wextra -O2 -I tiny -o tinyserver tiny/tiny.c tiny/csapp.c`

Tiny 웹서버 실행(로컬 테스트용)
- Tiny 실행(예: 8000 포트):
  - `make run-tiny PORT=8000` (권장)
  - 또는: `cd tiny && ./tinyserver 8000`
- 설명: 정적 파일과 CGI는 `tiny/` 디렉터리에서 서빙됩니다. `tiny/home.html`, `tiny/sample.mp4`, `tiny/cgi-bin/*`가 대상입니다.

프록시 실행
- 새 터미널에서 프록시 실행(예: 15213 포트):
  - `./proxy 15213`
- 특징: 요청당 연결을 닫는 HTTP/1.0 정책을 따르며, Part I에서는 순차(단일 연결씩) 처리합니다.

기본 동작 테스트(curl)
- 정적 HTML:
  - `curl -v --http1.0 -x http://localhost:15213 http://localhost:8000/home.html`
- 기대 사항:
  - 원서버로 나가는 요청라인: `GET /home.html HTTP/1.0` (CRLF)
  - 헤더: `Host` 보장, `User-Agent` 고정, `Connection: close`, `Proxy-Connection: close`
  - 본문은 Tiny 직접 요청 결과와 동일해야 합니다.
  - 직접 비교: `curl -v http://localhost:8000/home.html`

헤더 검증(nc로 원서버 흉내)
- 터미널 A(가짜 서버): `nc -l 9000`
- 터미널 B(클라이언트 via 프록시): `curl -v --http1.0 -x localhost:15213 http://localhost:9000/foo/bar`
- 터미널 A에서 관찰할 내용:
  - 첫 줄: `GET /foo/bar HTTP/1.0` (줄 끝 CRLF)
  - 정확히 한 개의 `Host:` 헤더
  - `User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3`
  - `Connection: close`
  - `Proxy-Connection: close`
  - 빈 줄(CRLF CRLF)로 헤더 종료

CGI 테스트
- Tiny의 간단한 CGI(adder):
  - `curl -v -x localhost:15213 "http://localhost:8000/cgi-bin/adder?1&2"`
- 기대: 합계를 보여주는 작은 HTML 페이지

바이너리 테스트(스트리밍 검증)
- mp4 등 바이너리 콘텐츠가 손상 없이 전달되는지 확인:
  - `curl -v -x localhost:15213 http://localhost:8000/sample.mp4 -o /tmp/proxy_sample.mp4`
  - `curl -v http://localhost:8000/sample.mp4 -o /tmp/direct_sample.mp4`
  - `cmp /tmp/proxy_sample.mp4 /tmp/direct_sample.mp4 && echo OK || echo MISMATCH`

에러 처리 테스트
- 미지원 메서드 → 501:
  - `curl -v -X POST -d a=b -x localhost:15213 http://localhost:8000/`
- 잘못된/미지원 URI → 400:
  - `curl -v -x localhost:15213 http://`
- 원서버 다운 → 502:
  - Tiny 중지 후: `curl -v -x localhost:15213 http://localhost:8000/home.html`

브라우저 테스트(선택)
- 브라우저의 HTTP 프록시를 `localhost:15213`로 설정합니다.
- 브라우저 캐시를 비활성화해야 프록시 동작을 정확히 검증할 수 있습니다.
- `http://localhost:8000/home.html`과 CGI 경로에 접속해 정상 표시를 확인합니다.

문제 해결(Troubleshooting)
- 인클루드 경로 에러:
  - 루트 디렉터리 기준 `-I .` 플래그로 `csapp.h`가 검색되게 합니다.
- SIGPIPE/조기 종료:
  - 프록시는 SIGPIPE를 무시합니다. EPIPE 로그는 클라이언트 조기 종료를 의미하며 프로세스는 계속 동작해야 합니다.
- CRLF 규칙:
  - 원서버로 보내는 모든 헤더 라인은 CRLF로 끝나야 하며, 빈 줄로 헤더를 종료해야 합니다.
- 긴 헤더 라인:
  - 프록시는 CRLF를 만날 때까지 누적 처리하므로 비정상적으로 긴 헤더도 깨지지 않고 전달됩니다.
- 포트 충돌:
  - `15213` 또는 `8000`이 사용 중이면 다른 포트를 사용하세요(예: `15215`, `8002`).

다음 단계
- Part II(동시성): `accept` 루프에서 연결마다 분리(detached) 스레드를 생성하고, `handle_client`를 재진입 가능하게 유지합니다.
- Part III(캐시): 객체 캐시(총 용량/객체 크기 제한, LRU 스타일 축출)와 리더-라이터 동기화를 추가합니다.

명령어 요약
- 한 번에 빌드/정리: `make clean && make all`
- Tiny 실행: `make run-tiny PORT=8000` (또는 `cd tiny && ../tinyserver 8000`)
- 프록시 실행: `./proxy 15213`
- 기본 테스트: `curl -v --http1.0 -x http://localhost:15213 http://localhost:8000/home.html`
