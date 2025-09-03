// Part III: Cache interface
#pragma once        // 헤더가 한 번만 포함되도록 함. 같은 번역 단위에서 중복 포함되어도 재정의 에러가 나지 않음
#include <stddef.h> // 표준 타입 size_t 등의 정의를 사용

#ifndef MAX_CACHE_SIZE           // 아직 MAX_CACHE_SIZE가 정의되지 않았다면
#define MAX_CACHE_SIZE (1 << 20) // 1 MiB: 캐시 총 용량(객체 바이트만) -> 기본값으로 정의
#endif
// 다른 곳에서 정해둔 값이 있으면 그것을 쓰고, 없으면 이 것을 쓰자. 설정의 유연성 증가

#ifndef MAX_OBJECT_SIZE
#define MAX_OBJECT_SIZE (100 << 10) // 100 KiB: 단일 객체 최대 크기
#endif

void cache_init(void);    // 캐시 전역 상태를 초기화
void cache_destroy(void); // 캐시를 해제. 모든 엔트리 제거, 동적 메모리 해제, 동기화 객체(락) 파괴
// key 문자열로 캐시 조회. HIT이면 data_out에 데이터 포인터, size_out에 크기를 채워 돌려줌
// - 반환값: HIT = 1, MISS = 0, 내부 오류 = -1
// - data_out는 내부 캐시 버퍼 포인터를 가리키게 됨
int cache_get(const char *key, char **data_out, size_t *size_out);
// key 문자열로 캐시에 새 객체 삽입. 기존 key가 있으면 교체
// - data는 key에 대응하는 객체 데이터(바이트 버퍼), size는 그 크기
// - size가 MAX_OBJECT_SIZE보다 크면 삽입하지 않고 무시
// - 내부적으로는 data를 복사하여 보관함
void cache_put(const char *key, const char *data, size_t size);
