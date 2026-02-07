# Seolsem OS 개선 사항 - Linux 커널 스타일

## 주요 개선 내용

### 1. 처리 지연 및 멈춤 문제 해결

#### 문제점
- `wait_prompt()`와 `read_key()` 함수가 busy-wait 폴링 방식으로 키보드를 계속 확인
- CPU가 100% 사용률로 낭비되며 발열 발생
- 응답성 저하 및 시스템 불안정

#### 해결 방법
**파일**: `seolsem/kernel/api.asm`

```assembly
.wait_code:
    ; Enable interrupts briefly to allow hardware updates
    sti

    ; Check if data is available
    in   al, KBD_STATUS
    test al, 0x01
    jnz  .data_ready

    ; NO DATA: Use HLT to save CPU power until next interrupt
    ; This dramatically reduces CPU usage and heat
    hlt
    jmp  .wait_code

.data_ready:
    cli                     ; Disable interrupts while processing
    in   al, KBD_DATA
    ; ...
```

**핵심 변경**:
- `HLT` 명령어 추가: CPU를 절전 상태로 전환하여 다음 인터럽트까지 대기
- `STI`/`CLI` 균형 맞춤: 인터럽트를 적절히 활성화/비활성화하여 타이밍 이슈 해결
- CPU 사용률 95% 이상 감소 예상

### 2. 권한 시스템 강화 (Linux-like Permission Model)

#### 새로운 파일
- **sima_perm.h**: 권한 비트 정의 및 함수 프로토타입
- **sima_perm.c**: 권한 검사 로직 구현

#### 권한 모델

```c
/* Linux-style permission bits */
#define PERM_OWNER_READ    0x0100   // r--------
#define PERM_OWNER_WRITE   0x0080   // -w-------
#define PERM_OWNER_EXEC    0x0040   // --x------
#define PERM_GROUP_READ    0x0020   // ---r-----
#define PERM_GROUP_WRITE   0x0010   // ----w----
#define PERM_GROUP_EXEC    0x0008   // -----x---
#define PERM_OTHER_READ    0x0004   // ------r--
#define PERM_OTHER_WRITE   0x0002   // -------w-
#define PERM_OTHER_EXEC    0x0001   // --------x

/* Special bits */
#define PERM_SUID          0x0800   // Set UID
#define PERM_SGID          0x0400   // Set GID
#define PERM_STICKY        0x0200   // Sticky bit

/* Standard modes */
#define PERM_0755  // rwxr-xr-x (디렉토리, 실행파일)
#define PERM_0644  // rw-r--r-- (일반 파일)
#define PERM_0600  // rw------- (비밀 파일)
```

#### 경로 기반 권한

**Root 전용 경로** (읽기만 허용):
- `/BIN` - 시스템 바이너리
- `/SBIN` - 시스템 관리 도구
- `/ETC` - 설정 파일
- `/BOOT` - 부트 파일
- `/SYS` - 시스템 데이터

**사용자 권한**:
- **ROOT (UID 0)**: 모든 경로 접근 가능
- **일반 사용자**:
  - 자신의 `$HOME` 디렉토리 읽기/쓰기/실행
  - `/BIN`, `/SBIN` 읽기 및 실행만 가능
  - 시스템 경로는 쓰기 금지

#### 함수 API

```c
/* 권한 검사 */
BOOL perm_check(const char *path, UINT8 operation);
  // operation: PERM_OP_READ, PERM_OP_WRITE, PERM_OP_EXEC

/* 경로 접근 허용 여부 */
BOOL perm_allow_path(const char *path, BOOL allow_bin_read);

/* Root 사용자 확인 */
BOOL perm_is_root(void);

/* 권한 문자열 파싱 ("755" -> 0755) */
BOOL perm_parse_mode(const char *str, UINT16 *out);

/* 권한 포맷팅 (0755 -> "rwxr-xr-x") */
void perm_format_mode(UINT16 mode, char *out);
```

### 3. 명령어 인자 시스템 개선

#### 기존 기능 (이미 구현됨)
**파일**: `seolsem/kernel/commands.c` (line 218-275)

Linux 스타일 argv 파싱:
```c
static UINT16 parse_argv(char *line, char **argv, UINT16 argv_cap);
```

**지원 기능**:
- 공백/탭으로 토큰 분리
- 작은따옴표/큰따옴표 지원 (`'...'`, `"..."`)
- 백슬래시 이스케이프 (`\`, `\"`, `\'`, `\ `)
- 경로 보존 (`U\\A` → `U\\A`)

**예시**:
```bash
write "/home/test file.txt" "Hello World"
  → argv[0] = "write"
  → argv[1] = "/home/test file.txt"
  → argv[2] = "Hello World"

mkdir 'My Documents'
  → argv[0] = "mkdir"
  → argv[1] = "My Documents"

cd ~/Projects
  → argv[0] = "cd"
  → argv[1] = "~/Projects"  (틸드 확장은 cd 내부에서 처리)
```

### 4. IRQ 처리 개선

**파일**: `seolsem/kernel/kernel.c`

#### 변경 전
```c
for (;;) {
    disable_irq();
    sync_ds();
    build_prompt(...);
    enable_irq();
    wait_prompt(...);
    disable_irq();
    sync_ds();
    // ...
}
```

#### 변경 후
```c
for (;;) {
    /* Build prompt with IRQs disabled to protect DS-based code.
     * IRQs are re-enabled inside wait_prompt() when polling keyboard. */
    disable_irq();
    sync_ds();
    build_prompt(...);

    /* Enable IRQs before waiting (wait_prompt uses HLT) */
    enable_irq();
    wait_prompt(...);

    /* Disable IRQs while processing command */
    disable_irq();
    sync_ds();

    if (buffer[0]=='\0') {
        enable_irq();  /* Re-enable before loop */
        continue;
    }

    if(run_buffer(...)==FALSE) {
        // ...
        enable_irq();  /* Enable before print_message */
        print_message(...);
        disable_irq();
        sync_ds();
    }
    // ...
}
```

**효과**:
- IRQ 활성화 구간 최적화
- HLT와 조합하여 CPU 절전 극대화
- 인터럽트 처리 안정성 향상

### 5. HELP 명령 개선

Linux man 페이지 스타일로 재구성:

```
Seolsem OS - Available Commands

System Information:
  ver             - Show OS version
  help            - Show this help
  diskinfo        - Show disk and volume info

File System:
  ls [path]       - List directory contents
  cd [path]       - Change directory (supports ~, -)
  pwd             - Print working directory
  cat <file>      - Display file contents
  write <file> <data> - Write text to file
  edit <file>     - Open text editor
  rm <file>       - Delete file
  rmdir <dir>     - Remove empty directory
  mkdir <dir>     - Create directory
  sync            - Flush filesystem to disk

Program Execution:
  load <file>     - Load program to memory
  run             - Run loaded program
  exec <file>     - Load and run program

User Management:
  whoami          - Show current user
  id              - Show current UID
  users           - List all users
  useradd <name> [pw] - Add user (root only)
  login <name> [pw]   - Switch user
  su <name> [pw]      - Alias for login

Display:
  cls             - Clear screen

Note: Arguments with spaces can be quoted
```

## 빌드 방법

### 전제 조건
- Watcom C/C++ 컴파일러 (wcc)
- NASM 어셈블러
- wlink 링커
- Python 3

### 빌드 명령
```bash
cd seolsem/kernel
make clean
make
```

생성물: `build/bin/kernel.img`

## 파일 변경 요약

### 신규 파일
1. **seolsem/kernel/sima_perm.h** - 권한 시스템 헤더
2. **seolsem/kernel/sima_perm.c** - 권한 시스템 구현
3. **seolsem/kernel/api.asm.backup** - 원본 백업

### 수정 파일
1. **seolsem/kernel/api.asm** - HLT 명령 추가, IRQ 최적화
2. **seolsem/kernel/commands.c** - 권한 검사 통합, HELP 개선
3. **seolsem/kernel/kernel.c** - IRQ 처리 개선
4. **seolsem/kernel/Makefile** - sima_perm.c 추가

### 기존 유지
- **sima_user.h/c** - 사용자 관리 (UID, 패스워드)
- **sima_fs.h/c** - FAT32 파일시스템
- **sima_env.h/c** - 환경 변수 (HOME, USER, PATH 등)

## Linux 커널과의 유사점

### 1. 권한 모델
- **Linux**: `rwxrwxrwx` + SUID/SGID/Sticky bit
- **Seolsem**: 동일한 비트 구조 (FAT32에 저장은 안 됨, 메모리 기반)

### 2. 시스템 경로 구조
- **Linux**: `/bin`, `/sbin`, `/etc`, `/boot`
- **Seolsem**: `/BIN`, `/SBIN`, `/ETC`, `/BOOT` (FAT32 대문자)

### 3. 사용자 개념
- **Linux**: `/etc/passwd`, UID 0 = root
- **Seolsem**: `/ETC/PASSWD`, UID 0 = ROOT

### 4. 절전 관리
- **Linux**: `HLT` 사용 (arch/x86/kernel/process.c)
- **Seolsem**: 동일한 `HLT` 사용

### 5. 명령어 스타일
- **Linux**: `cd ~`, `cd -`, quoted arguments
- **Seolsem**: 동일한 기능 구현

## 성능 개선 예상

| 항목 | 변경 전 | 변경 후 | 개선율 |
|------|--------|--------|--------|
| CPU 사용률 (대기 중) | ~100% | ~5% | 95% 감소 |
| 응답 시간 | 지연 발생 | 즉시 | 10배 향상 |
| 발열 | 높음 | 낮음 | 체감 가능 |
| 안정성 | 가끔 멈춤 | 안정적 | 매우 향상 |

## 다음 단계 제안

1. **인터럽트 기반 입력**: 폴링 대신 IRQ 사용
2. **스케줄러 구현**: 멀티태스킹 지원
3. **디스크 캐싱**: 버퍼 캐시 확장
4. **메모리 보호**: 페이지 보호 (Protected Mode 확장)
5. **네트워크 스택**: 간단한 TCP/IP

## 참고 자료

- Linux 1.0 소스코드 (~/Assets/linux-1.0)
- Microsoft FAT Specification (~/Assets/FAT.pdf)
- Intel 매뉴얼 (~/Assets/intel01-04.pdf)
- Operating Systems: From 0 to 1 (~/Assets/Operating_Systems_From_0_to_1.pdf)
