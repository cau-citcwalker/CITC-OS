# CITC OS 개발 학습 기록

> Linux 커널 기반 커스텀 OS를 만들면서 배운 모든 것을 정리한 문서.
> 각 수업(Class)별로 핵심 개념, 구현 내용, 겪었던 문제와 해결 과정을 기록.

---

## 목차

### Phase 0: 기반 구축

- [Class 1: Init 시스템 (PID 1)](#class-1-init-시스템-pid-1)
- [Class 11: 입력 시스템 + 윈도우 관리자](#class-11-입력-시스템--윈도우-관리자)
- [Class 12: CITC Display Protocol (Wayland 개념)](#class-12-citc-display-protocol-wayland-개념)
- [Class 13: PE 로더 (Windows .exe 실행)](#class-13-pe-로더-windows-exe-실행)
- [Class 14: 터미널 에뮬레이터 (PTY + CDP)](#class-14-터미널-에뮬레이터-pty--cdp)
- [Class 15: 커스텀 쉘 (citcsh)](#class-15-커스텀-쉘-citcsh)
- [Class 16: Win32 API (kernel32.dll 핵심 함수)](#class-16-win32-api-kernel32dll-핵심-함수)
- [Class 17: 데스크탑 셸 (태스크바 + 앱 런처)](#class-17-데스크탑-셸-태스크바--앱-런처)
- [Class 18: 부팅 환경 개선 (듀얼 콘솔 + 시리얼 쉘)](#class-18-부팅-환경-개선-듀얼-콘솔--시리얼-쉘)
- [Class 19: 소켓 활성화 (Socket Activation)](#class-19-소켓-활성화-socket-activation)
- [Class 20: IPC 메시지 버스 (간소화 D-Bus)](#class-20-ipc-메시지-버스-간소화-d-bus)
- [Class 21: .desktop 파일 지원 + Phase 1 완료](#class-21-desktop-파일-지원--phase-1-완료)

### Phase 2: WCL 기초 (Windows Compatibility Layer)

- [Class 22: 기존 코드 버그 수정 + 개선](#class-22-기존-코드-버그-수정--개선)
- [Class 23: NT 에뮬레이션 (ntdll + Object Manager)](#class-23-nt-에뮬레이션-ntdll--object-manager)
- [Class 24: 레지스트리 v0.1 (파일 기반)](#class-24-레지스트리-v01-파일-기반)
- [Class 25: kernel32 확장 + 다중 DLL 지원](#class-25-kernel32-확장--다중-dll-지원)
- [Class 27: last_error 버그 수정 + API 테스트 확장](#class-27-last_error-버그-수정--api-테스트-확장)

### Phase 3: Windows GUI 지원

- [Class 28: 윈도우 관리 + GDI 렌더링 (user32 + gdi32)](#class-28-윈도우-관리--gdi-렌더링-user32--gdi32)
- [Class 30: 입력 통합 + QEMU 실제 테스트](#class-30-입력-통합--qemu-실제-테스트)
- [Class 31: user32/gdi32 확장 (Phase 3 보강)](#class-31-user32gdi32-확장-phase-3-보강)

### Phase 4: DirectX & 게이밍 (소프트웨어 래스터라이저)

- [Class 32: COM + DirectX 타입 기반](#class-32-com--directx-타입-기반)
- [Class 33: DXGI 구현 (Factory + SwapChain)](#class-33-dxgi-구현-factory--swapchain)
- [Class 34: D3D11 Device + Context](#class-34-d3d11-device--context)
- [Class 35: 소프트웨어 래스터라이저 + Hello Triangle](#class-35-소프트웨어-래스터라이저--hello-triangle)

### 부록

- [공통 교훈: 자주 발생한 패턴](#공통-교훈)

---

## Class 1: Init 시스템 (PID 1)

**파일:** `system/citcinit/src/main.c`

### 핵심 개념

**PID 1이란?**
Linux 커널이 부팅을 마치면 가장 먼저 실행하는 유저 프로그램. 모든 프로세스의 조상이다.

**PID 1의 5가지 책임:**
1. 가상 파일시스템 마운트 (`/proc`, `/sys`, `/dev`)
2. 시스템 초기 설정 (호스트네임, 콘솔)
3. 시스템 서비스 시작
4. 고아 프로세스 회수 (좀비 방지)
5. 시스템 종료 처리

**가상 파일시스템:**

| 경로 | 타입 | 역할 |
|------|------|------|
| `/proc` | procfs | 프로세스 정보 (`ps`, `top`이 읽음) |
| `/sys` | sysfs | 하드웨어 장치 정보 계층 구조 |
| `/dev` | devtmpfs | 장치 파일 (하드웨어를 파일처럼 접근) |
| `/dev/pts` | devpts | 가상 터미널 (SSH, 터미널 에뮬레이터) |
| `/run` | tmpfs | 런타임 데이터 (RAM, 재부팅 시 삭제) |
| `/tmp` | tmpfs | 임시 파일 |

**장치 파일 (mknod):**
```c
// mknod(경로, 모드|타입, makedev(주번호, 부번호))
mknod("/dev/console", 0600 | S_IFCHR, makedev(5, 1));
mknod("/dev/null",    0666 | S_IFCHR, makedev(1, 3));
```
- 주 번호(major): 드라이버 종류 식별
- 부 번호(minor): 같은 드라이버의 개별 장치 식별

**좀비 프로세스와 PID 1:**
```c
// 모든 프로세스가 종료되면 커널은 부모에게 SIGCHLD를 보냄.
// 부모가 waitpid()를 호출해야 좀비가 해소됨.
// 부모가 먼저 죽으면 → 고아 프로세스 → PID 1이 새 부모가 됨.
// PID 1이 waitpid()를 안 하면 → 좀비 누적 → 시스템 리소스 고갈
while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
    svc_notify_exit(pid, status);  // 서비스 관리자에 알림
}
```

**콘솔 설정 (stdin/stdout/stderr):**
```c
// 파일 디스크립터 0, 1, 2를 /dev/console에 연결
int fd = open("/dev/console", O_RDWR);
dup2(fd, 0);  // stdin
dup2(fd, 1);  // stdout
dup2(fd, 2);  // stderr
```

### 배운 것
- `mount()` 시스템 콜과 가상 파일시스템의 차이
- `fork()` + `execve()` 프로세스 생성 패턴
- `SIGCHLD` 핸들링과 좀비 프로세스 방지
- `-static` 링킹의 중요성 (initramfs에는 공유 라이브러리가 없음)

---

## Class 2: 서비스 관리자

**파일:** `system/citcinit/src/service.h`, `service.c`

### 핵심 개념

**서비스 = 백그라운드 프로세스 (데몬)**

**서비스 생명주기:**
```
STOPPED ──(start)──→ STARTING ──(ready)──→ RUNNING
   ▲                    │                     │
   │                    │(fail)               │(crash/stop)
   │                    ▼                     ▼
   └────────────── FAILED ←───────────── STOPPING
```

**서비스 3가지 타입:**

| 타입 | 동작 | 예시 |
|------|------|------|
| SIMPLE | fork하면 바로 "실행 중" | syslogd, 대부분의 데몬 |
| ONESHOT | 실행 완료 후 "완료" | 네트워크 설정 스크립트 |
| NOTIFY | 서비스가 준비됨을 직접 알림 | 데이터베이스 서버 |

**의존성 기반 시작 (위상 정렬):**
```
syslog → (없음)          → 1번째
klogd  → {syslog}        → syslog 다음
network → {syslog}       → syslog 다음
```
- DFS(깊이 우선 탐색)로 의존성 트리 순회
- 의존 대상이 RUNNING이 될 때까지 대기

**왜 포그라운드 모드(-n)?**
```
데몬이 자체 fork() → PID 변경 → init이 추적 불가
해결: -n 플래그로 포그라운드 실행 요청
이것이 systemd, s6, runit의 표준 방식
```

### 설계 결정
- 배열 기반 서비스 저장 (링크드 리스트 대신)
  - 캐시 효율성이 좋음
  - malloc 실패 걱정 없음
  - OS 초기에 힙이 불안정할 수 있음

---

## Class 3: 실제 서비스 연결

**파일:** `system/citcinit/services/syslog.conf`, `klogd.conf`

### 핵심 개념

**busybox syslogd:**
- Unix 표준 로그 시스템
- `/dev/log` 소켓으로 로그 수집
- `-n` 플래그: 포그라운드 실행 (init 관리용)

**busybox klogd:**
- 커널 로그 수집 (`/proc/kmsg` 읽기)
- syslogd에 의존 (로그를 syslogd로 전달)

### 배운 것
- 서비스 간 의존성 관계의 실제 구현
- 서비스 자동 재시작 메커니즘 (crash → restart_count 확인 → 재시작 or FAILED)

---

## Class 4: 설정 파일 기반 서비스 로드

**파일:** `system/citcinit/src/config.h`, `config.c`

### 핵심 개념

**설정 파일 포맷:**
```ini
# /etc/citc/services/syslog.conf
name=syslog
exec=/sbin/syslogd
type=simple
restart=yes
args=-n
depends=
```

**디렉토리 스캔:**
```c
DIR *dir = opendir("/etc/citc/services");
struct dirent *ent;
while ((ent = readdir(dir)) != NULL) {
    // .conf 파일만 처리
    if (strstr(ent->d_name, ".conf"))
        parse_config(path);
}
```

**파싱 패턴:**
- `fgets()` → 한 줄씩 읽기
- `#`으로 시작 → 주석
- `=` 위치 찾기 → key/value 분리
- `trim()` → 앞뒤 공백 제거

### 배운 것
- `opendir()`/`readdir()`/`closedir()` 디렉토리 순회
- INI 스타일 설정 파일 파싱
- 하드코딩 → 설정 파일 전환의 이점 (재컴파일 없이 서비스 추가/제거)

---

## Class 5: QEMU 부팅 & initramfs

**파일:** `tools/mkrootfs/build-initramfs.sh`, `tools/run-qemu.sh`, `tools/mkiso/build-iso.sh`

### 핵심 개념

**부팅 순서:**
```
BIOS/UEFI → GRUB → Linux 커널 → initramfs 해제 → /sbin/init (PID 1)
```

**initramfs란?**
- 커널이 메모리에 풀어놓는 초기 RAM 파일시스템
- cpio 아카이브를 gzip 압축한 것
- 루트 파일시스템이 마운트되기 전에 필요한 최소 환경

**initramfs 생성 과정:**
```bash
# 1. FHS 디렉토리 구조 생성
mkdir -p {bin,sbin,dev,etc,proc,sys,tmp,run,var,usr/{bin,lib},mnt}

# 2. citcinit 설치 → /sbin/init
# 3. busybox 설치 → /bin/busybox + 심볼릭 링크
# 4. 설정 파일 복사
# 5. cpio 아카이브 + gzip 압축
find . | cpio -o -H newc | gzip > initramfs.cpio.gz
```

**QEMU 3가지 모드:**

| 모드 | 플래그 | 용도 |
|------|--------|------|
| 텍스트 | `-nographic` | 시리얼 콘솔만 (빠름) |
| 그래픽 | `-vga std -serial stdio` | VGA 출력 + 시리얼 입력 |
| 디버그 | `-s -S` | GDB 연결 대기 |

**KVM 가속:**
```bash
if [ -c /dev/kvm ]; then
    QEMU_OPTS+=(-enable-kvm -cpu host)
    # 하드웨어 가상화 → 네이티브에 가까운 속도
fi
```

### 문제 & 해결

**문제: NTFS에서 initramfs 생성 실패**
- 원인: NTFS는 mknod (장치 파일), 심볼릭 링크, Unix 퍼미션을 지원하지 않음
- 해결: `/tmp` (WSL의 ext4)에서 작업 후 결과물만 Windows로 복사
```bash
WORK_DIR="/tmp/citcos-initramfs-$$"  # ext4에서 작업!
```

**문제: "not found" 에러**
- 원인: 동적 링킹된 바이너리 → 공유 라이브러리(.so)가 initramfs에 없음
- 해결: 모든 바이너리를 `-static`으로 빌드

---

## Class 6: 네트워킹

**파일:** `system/citcinit/services/network.conf`, `scripts/network-setup.sh`

### 핵심 개념

**네트워크 초기화 순서:**
1. loopback 활성화 (`lo` = 127.0.0.1)
2. 이더넷 인터페이스 탐색 (`/sys/class/net/` 스캔)
3. 인터페이스 활성화 (`ip link set eth0 up`)
4. DHCP로 IP 주소 획득 (`udhcpc`)

**DHCP 과정:**
```
클라이언트 → DISCOVER (브로드캐스트)
서버       → OFFER (IP 제안)
클라이언트 → REQUEST (IP 요청)
서버       → ACK (확인)
```

**QEMU 네트워크:**
```
QEMU 게스트 (10.0.2.15) ←→ QEMU NAT ←→ 호스트 (10.0.2.2) ←→ 인터넷
```
- `-netdev user,id=net0`: QEMU 내장 NAT
- `-device virtio-net-pci`: 가상 NIC

**udhcpc 옵션:**
```bash
udhcpc -i "$IFACE" \
       -s /etc/citc/scripts/udhcpc-default.script \  # 이벤트 핸들러
       -n \   # 실패 시 즉시 종료
       -q \   # IP 받으면 종료 (oneshot 적합)
       -t 5   # 최대 5번 시도
```

**oneshot 서비스로 구현한 이유:**
- 네트워크 설정은 한 번 하면 끝
- 스크립트 실행 → IP 획득 → 종료 → "완료" 상태
- 다른 서비스가 network에 의존 가능

### 배운 것
- `/sys/class/net/` 에서 네트워크 인터페이스 탐색
- DHCP 프로토콜의 4단계 핸드셰이크
- oneshot 서비스의 활용 (설정 스크립트에 적합)

---

## Class 7: 패키지 매니저 (로컬)

**파일:** `system/citcpkg/src/main.c`, `package.h`, `package.c`

### 핵심 개념

**패키지 포맷 (.cpkg):**
```
hello-1.0.cpkg (tar.gz 아카이브)
├── PKGINFO           ← 메타데이터
│   name=hello
│   version=1.0
│   description=Hello World
│   depends=
└── data/             ← 실제 파일 (루트에 추출)
    └── usr/
        └── bin/
            └── hello
```

**설치 과정:**
```
1. /tmp에 압축 해제
2. PKGINFO 파싱
3. 의존성 확인 (설치됐는지)
4. data/ → / 복사
5. /var/lib/citcpkg/installed/{name}/ 에 기록
```

**스마트 감지:**
```c
if (argv[2][0] == '/' || argv[2][0] == '.')
    pkg_install(argv[2]);   // 로컬 파일
else
    repo_install(argv[2]);  // 원격 저장소
```

**명령어:**

| 명령 | 동작 |
|------|------|
| `install <경로>` | 로컬 패키지 설치 |
| `install <이름>` | 원격 저장소에서 설치 |
| `remove <이름>` | 패키지 제거 |
| `list` | 설치된 패키지 목록 |
| `info <이름>` | 패키지 상세 정보 |
| `update` | 저장소 인덱스 갱신 |
| `search [키워드]` | 패키지 검색 |

### 배운 것
- tar.gz 아카이브 구조와 활용
- 패키지 메타데이터 설계 (PKGINFO)
- 설치 기록 관리 (파일 목록 저장 → 깔끔한 제거 가능)
- Unix 철학: 외부 도구(tar, cp, rm) 활용

---

## Class 8: 원격 저장소 & 의존성 해결

**파일:** `system/citcpkg/src/repo.h`, `repo.c`

### 핵심 개념

**저장소 구조:**
```
http://10.0.2.2:8080/       ← Docker + nginx
├── PKGINDEX                ← 패키지 목록 (인덱스)
├── hello-1.0.cpkg          ← 패키지 파일들
└── greeting-1.0.cpkg
```

**PKGINDEX 포맷:**
```
name=hello
version=1.0
description=Hello World 프로그램
depends=
filename=hello-1.0.cpkg

name=greeting
version=1.0
description=인사 프로그램
depends=hello
filename=greeting-1.0.cpkg
```

**의존성 해결 (DFS):**
```
greeting 설치 요청
  → depends=hello 발견
  → hello 먼저 설치 (재귀)
  → greeting 설치

방문 배열(visited[])로 중복/순환 방지
```

**`citcpkg update` 흐름:**
```
/etc/citcpkg/repo.conf 에서 URL 읽기
→ wget으로 PKGINDEX 다운로드
→ /var/lib/citcpkg/PKGINDEX에 저장
```

### 배운 것
- HTTP 기반 패키지 저장소 설계
- DFS로 의존성 트리 순회 (재귀적 설치)
- `wget` + `system()`을 이용한 다운로드
- Docker + nginx로 패키지 서버 구축

---

## Class 9: 프레임버퍼 그래픽

**파일:** `display/fbdraw/src/fbdraw.c`, `font8x8.h`

### 핵심 개념

**프레임버퍼란?**
- GPU 메모리의 픽셀 데이터를 파일로 노출한 것
- `/dev/fb0`를 mmap하면 직접 픽셀 쓰기 가능

**초기화:**
```c
int fd = open("/dev/fb0", O_RDWR);
ioctl(fd, FBIOGET_VSCREENINFO, &vinfo);  // 해상도, bpp 조회
ioctl(fd, FBIOGET_FSCREENINFO, &finfo);  // stride 등 조회
mem = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
```

**픽셀 오프셋 계산:**
```c
// 핵심 공식: y * stride + x * bytes_per_pixel
offset = y * finfo.line_length + x * (vinfo.bits_per_pixel / 8);
```

**비트 깊이별 처리:**

| bpp | 바이트 | 색상 수 | 처리 방법 |
|-----|--------|---------|----------|
| 32 | 4 | 16M+ | uint32_t 캐스팅 (간단) |
| 24 | 3 | 16M | 바이트 단위 개별 쓰기 |
| 16 | 2 | 65K | RGB565 패킹 |

**8x8 비트맵 폰트:**
```c
// font8x8_basic[문자코드][행] = 8비트 비트맵
uint8_t bits = font8x8_basic['A'][row];
// 각 비트가 1이면 해당 위치에 픽셀 그리기
if (bits & (1 << col)) draw_pixel(x, y);
```

### 문제 & 해결

**문제: QEMU에서 그래픽이 안 보임 (mmap만으로는 갱신 안 됨)**
- 원인: DRM의 fbdev 에뮬레이션에서는 mmap 변경이 자동 반영되지 않을 수 있음
- 해결: 3중 flush 전략
```c
static void fb_flush(void) {
    msync(fb.mem, fb.size, MS_SYNC);          // 1. mmap 동기화
    lseek(fb.fd, 0, SEEK_SET);
    write(fb.fd, fb.mem, fb.size);             // 2. write() 직접 쓰기
    ioctl(fb.fd, FBIOPAN_DISPLAY, &fb.vinfo);  // 3. 디스플레이 갱신
}
```

### 배운 것
- 프레임버퍼 디바이스의 구조 (`/dev/fb0`)
- mmap을 통한 메모리 매핑 그래픽
- stride(pitch)와 bpp에 따른 오프셋 계산
- 비트맵 폰트 렌더링 원리

---

## Class 9.5: Shutdown 명령어

**파일:** `system/citcinit/src/shutdown.c`

### 핵심 개념

**시그널 프로토콜 (shutdown → citcinit):**

| 시그널 | 동작 | 명령 |
|--------|------|------|
| SIGTERM | 전원 끄기 (power off) | `shutdown`, `poweroff` |
| SIGINT | 재부팅 (restart) | `shutdown -r`, `reboot` |
| SIGUSR1 | 시스템 정지 (halt) | `shutdown -h`, `halt` |

**argv[0] 트릭:**
```
/sbin/shutdown  ← 실제 바이너리
/sbin/reboot    → 심볼릭 링크 → /sbin/shutdown
/sbin/poweroff  → 심볼릭 링크 → /sbin/shutdown
/sbin/halt      → 심볼릭 링크 → /sbin/shutdown
```
```c
const char *prog = basename(argv[0]);
if (strcmp(prog, "reboot") == 0)   mode = MODE_REBOOT;
if (strcmp(prog, "poweroff") == 0) mode = MODE_POWEROFF;
if (strcmp(prog, "halt") == 0)     mode = MODE_HALT;
```
busybox도 같은 원리: 하나의 바이너리, 수백 개의 심볼릭 링크.

**citcinit의 종료 과정:**
```
시그널 수신 → shutdown_requested = 1
→ svc_stop_all() (서비스 먼저 정지)
→ kill(-1, SIGTERM) (모든 프로세스에 정상 종료 요청)
→ 3초 대기
→ kill(-1, SIGKILL) (남은 프로세스 강제 종료)
→ sync() (파일시스템 동기화)
→ reboot(cmd) (커널에 재부팅/종료 요청)
```

### 문제 & 해결

**문제: `shutdown_cmd` 비교에서 sign-compare 경고**
```
comparison of integer expressions of different signedness: 'int' and 'unsigned int'
```
- 원인: `LINUX_REBOOT_CMD_HALT = 0xCDEF0123` → INT_MAX(0x7FFFFFFF) 초과
- `volatile int`와 비교하면 부호 불일치
- 해결: `static volatile unsigned int shutdown_cmd`로 변경

### 배운 것
- PID 1에 시그널을 보내는 방식의 시스템 종료
- argv[0] 트릭 (하나의 바이너리, 여러 이름)
- `kill(1, signal)` — PID 1에 시그널 전송
- 안전한 종료 순서 (SIGTERM → 대기 → SIGKILL → sync → reboot)
- unsigned 리터럴과 signed 변수 비교 시 주의

---

## Class 10: DRM/KMS 그래픽

**파일:** `display/drmdraw/src/drmdraw.c`

### 핵심 개념

**DRM/KMS 아키텍처:**
```
모니터 ← Connector ← Encoder ← CRTC ← Framebuffer
(물리)    (포트)      (변환기)   (엔진)   (픽셀 데이터)
```

| 컴포넌트 | 역할 | 예시 |
|----------|------|------|
| Connector | 물리적 출력 포트 | HDMI, DP, VGA |
| Encoder | 픽셀 → 디스플레이 신호 변환 | TMDS, LVDS |
| CRTC | 프레임버퍼 스캔아웃 엔진 | "CRT Controller" |
| Framebuffer | GPU 메모리의 픽셀 데이터 | DRM FB 오브젝트 |
| Dumb Buffer | CPU 접근 가능한 단순 버퍼 | GPU 가속 없음 |

**fbdev vs DRM 비교:**

| 항목 | fbdev (`/dev/fb0`) | DRM (`/dev/dri/card0`) |
|------|-------------------|----------------------|
| 역사 | 1990년대 | 2000년대~현재 |
| 해상도 | 부팅 시 고정 | 런타임 변경 가능 |
| 모드 감지 | 수동 설정 | EDID 자동 감지 |
| 더블 버퍼링 | 불가능/불안정 | 네이티브 지원 |
| 페이지 플립 | 없음 → 티어링 | 원자적 → 티어링 없음 |
| GPU 가속 | 없음 | GEM/DMA-BUF 가능 |
| 다중 모니터 | 별도 `/dev/fb0,1..` | 하나의 fd로 관리 |
| 현대 앱 | 사용하지 않음 | Wayland/X11이 사용 |

**Dumb Buffer 생성 (4단계):**
```
1. CREATE_DUMB  → GPU 메모리에 버퍼 할당 (handle 획득)
2. ADDFB        → DRM 프레임버퍼로 등록 (fb_id 획득)
3. MAP_DUMB     → mmap 오프셋 획득
4. mmap()       → CPU 접근 가능한 포인터 획득
```

**더블 버퍼링:**
```
Front buffer: 현재 화면에 표시 중
Back buffer:  다음 프레임을 그리는 중

그리기 완료 → SETCRTC로 두 버퍼 교환 (page flip)
→ 그리는 도중의 불완전한 프레임이 보이지 않음!
```

**DRM 2-pass ioctl 패턴:**
```c
// 1st pass: 크기만 조회
struct drm_mode_card_res res = {0};
ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
// → res.count_connectors, res.count_crtcs 등이 채워짐

// 배열 할당
conn_ids = calloc(res.count_connectors, sizeof(uint32_t));
crtc_ids = calloc(res.count_crtcs, sizeof(uint32_t));
// ...

// 포인터 설정
res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
res.crtc_id_ptr = (uint64_t)(unsigned long)crtc_ids;
// ...

// 2nd pass: 실제 데이터 채우기
ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res);
```

**포인터 캐스팅이 필요한 이유:**
```c
res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
```
커널 ioctl 인터페이스는 포인터를 64비트 정수로 전달한다.
32비트/64비트 호환성을 위해 같은 struct를 양쪽에서 사용하기 때문.

### 문제 & 해결 (총 4개)

#### 문제 1: `DRM_MODE_CONNECTED` 미정의 + `font8x8` 미정의
```
error: 'DRM_MODE_CONNECTED' undeclared
error: 'font8x8' undeclared
```
- 원인 1: 유저스페이스 DRM 헤더에 해당 상수가 없을 수 있음
- 해결 1: `#ifndef` 가드로 수동 정의
```c
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED         1
#define DRM_MODE_DISCONNECTED      2
#define DRM_MODE_UNKNOWNCONNECTION 3
#endif
```
- 원인 2: include 경로 오류 + 배열 이름이 `font8x8_basic`
- 해결 2: 경로를 `../../fbdraw/src/font8x8.h`로 수정, 배열명 변경

#### 문제 2: GETRESOURCES 2nd pass EFAULT
```
DRM_IOCTL_MODE_GETRESOURCES (2nd): Bad address
```
- **원인:** `connector_id_ptr`과 `crtc_id_ptr`만 설정하고, `encoder_id_ptr`과 `fb_id_ptr`은 NULL로 둠
- **핵심 규칙:** `count > 0`인 **모든** 배열에 유효한 포인터를 전달해야 함!
- 해결:
```c
// 4개 배열 모두 할당하고 포인터 설정
conn_ids    = calloc(res.count_connectors, sizeof(uint32_t));
crtc_ids    = calloc(res.count_crtcs, sizeof(uint32_t));
enc_ids_res = calloc(res.count_encoders ? res.count_encoders : 1, sizeof(uint32_t));
fb_ids      = calloc(res.count_fbs ? res.count_fbs : 1, sizeof(uint32_t));

res.connector_id_ptr = (uint64_t)(unsigned long)conn_ids;
res.crtc_id_ptr      = (uint64_t)(unsigned long)crtc_ids;
res.encoder_id_ptr   = (uint64_t)(unsigned long)enc_ids_res;
res.fb_id_ptr        = (uint64_t)(unsigned long)fb_ids;
```

#### 문제 3: "연결된 디스플레이를 찾을 수 없습니다"
디버그 출력: `커넥터 35: connection=1, modes=1, encoders=1`
→ 커넥터는 연결됨, 모드도 있음. 그런데 2nd pass 이후 메시지가 안 나옴.

- **원인:** GETCONNECTOR 2nd pass에서도 같은 규칙! `props_ptr`과 `prop_values_ptr`이 NULL인데 `count_props > 0`
- **해결:** GETRESOURCES와 동일하게 모든 배열 할당
```c
modes       = calloc(conn.count_modes, sizeof(struct drm_mode_modeinfo));
enc_ids     = calloc(conn.count_encoders ? conn.count_encoders : 1, sizeof(uint32_t));
props       = calloc(conn.count_props ? conn.count_props : 1, sizeof(uint32_t));
prop_values = calloc(conn.count_props ? conn.count_props : 1, sizeof(uint64_t));

conn.modes_ptr       = (uint64_t)(unsigned long)modes;
conn.encoders_ptr    = (uint64_t)(unsigned long)enc_ids;
conn.props_ptr       = (uint64_t)(unsigned long)props;
conn.prop_values_ptr = (uint64_t)(unsigned long)prop_values;
```

#### 문제 4: QEMU 가상 디스플레이 연결 상태
- QEMU bochs-drm은 `DRM_MODE_UNKNOWNCONNECTION`(3)을 보고할 수 있음
- 가상 디스플레이에는 물리적 연결 감지가 없기 때문
- 해결: `DISCONNECTED`(2)만 건너뛰고, `CONNECTED`(1)와 `UNKNOWNCONNECTION`(3) 모두 허용

### 배운 것
- DRM/KMS의 Connector → Encoder → CRTC → Framebuffer 파이프라인
- 2-pass ioctl 패턴과 **모든 배열 포인터 필수** 규칙
- Dumb buffer 생성 4단계 (CREATE → ADDFB → MAP → mmap)
- 더블 버퍼링의 원리와 구현
- QEMU 가상 디스플레이의 특수 동작

---

## Class 11: 입력 시스템 + 윈도우 관리자

**파일:** `display/compositor/src/compositor.c`, `display/compositor/Makefile`

### 핵심 개념

**목표:** DRM/KMS 위에서 마우스 커서가 움직이고, 윈도우를 클릭/드래그하고, 키보드 입력이 포커스된 윈도우에 표시되는 대화형 컴포지터.

**Linux 입력 시스템 (evdev):**
```
키보드/마우스 → 커널 드라이버 → /dev/input/eventX → struct input_event
```

- evdev = "event device" — Linux 표준 입력 인터페이스
- 모든 입력 장치가 `/dev/input/eventX` 파일로 노출됨
- `struct input_event { time, type, code, value }` 를 `read()`로 읽음

**이벤트 타입:**

| type | 의미 | 예시 |
|------|------|------|
| EV_KEY (0x01) | 키/버튼 누름/해제 | KEY_A=30, BTN_LEFT=0x110 |
| EV_REL (0x02) | 상대 이동 | REL_X, REL_Y (일반 마우스) |
| EV_ABS (0x03) | 절대 좌표 | ABS_X, ABS_Y (태블릿/터치) |
| EV_SYN (0x00) | 이벤트 동기화 | 한 묶음의 이벤트 끝 표시 |

**장치 판별 (EVIOCGBIT ioctl):**
```c
unsigned long evbits[NLONGS(EV_MAX)] = {0};
ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits);
// 어떤 이벤트 타입을 지원하는지 비트맵으로 조회

// 절대좌표 장치의 범위 조회
struct input_absinfo abs_x;
ioctl(fd, EVIOCGABS(ABS_X), &abs_x);
// abs_x.maximum = 32767 (일반적)
```

**poll() 기반 이벤트 루프:**
```c
struct pollfd fds[MAX_FDS];
// 각 입력 장치의 fd를 등록
for (int i = 0; i < num_inputs; i++) {
    fds[i].fd = inputs[i].fd;
    fds[i].events = POLLIN;
}

while (running) {
    poll(fds, num_inputs, 16);  // 16ms 타임아웃 (~60fps)
    for (각 fd) {
        if (fds[i].revents & POLLIN) {
            struct input_event ev;
            read(fd, &ev, sizeof(ev));
            // 이벤트 처리
        }
    }
    if (need_redraw) render_frame();
}
```

**윈도우 관리:**
```
윈도우 = { x, y, w, h, title, text_buf, focused, visible }
Z-order: 배열 뒤쪽이 화면 위에 그려짐
포커스: 클릭한 윈도우가 키보드 입력을 받음
드래그: 타이틀바 클릭 + 마우스 이동 → 윈도우 위치 변경
```

**컴포지팅 (Painter's Algorithm):**
```
1. 배경 (그라디언트)
2. 윈도우들 (뒤에서 앞으로 순서)
3. 커서 (항상 최상위)
```

**Shift 키 처리:**
```c
// evdev는 물리 키 이벤트만 전달 → modifier 상태를 직접 추적
static int shift_held;

if (ev->code == KEY_LEFTSHIFT || ev->code == KEY_RIGHTSHIFT)
    shift_held = (ev->value != 0);  // 1=누름, 0=해제

// 문자 변환 시 shift_held에 따라 keymap_lower/upper 선택
char ch = shift_held ? keymap_upper[code] : keymap_lower[code];
```

### 문제 & 해결 (총 5개)

#### 문제 1: snprintf format-truncation 경고

```
error: '%s' directive output may be truncated [-Werror=format-truncation=]
```

- `path[128]`에 `/dev/input/` (11바이트) + `d_name` (최대 255바이트) 결합
- `-Werror`에서 경고가 에러로 승격
- **해결:** `path[280]`으로 버퍼 확대

#### 문제 2: 마우스 입력 없음 (EV_ABS 미지원)

- 컴포지터가 `EV_REL`(상대 이동)만 체크하여 마우스를 감지
- QEMU USB 태블릿은 `EV_ABS`(절대 좌표) 사용 → 감지 안 됨
- **해결:** `EV_ABS` 감지 추가 + `EVIOCGABS`로 좌표 범위 조회 + 화면 좌표 스케일링

```c
// 절대좌표 → 화면좌표 변환
screen_x = (abs_value * screen_width) / abs_max_x;
```

#### 문제 3: 커널 모듈 로드 전부 실패 (버전 불일치)

빌드 스크립트에서 `KVER="$(uname -r)"`로 커널 버전을 감지했는데:

- WSL 안에서 `uname -r` = `6.6.87.2-microsoft-standard-WSL2` (WSL 커널)
- QEMU에서 부팅하는 커널 = `6.8.0-100-generic` (Ubuntu 커널)
- **WSL 커널의 모듈을 Ubuntu 커널에 로딩** → 버전 불일치 → `insmod` 전부 실패!
- 에러 메시지는 `2>/dev/null`로 숨겨져서 "이미 로딩됨"으로 오인

**해결 (2단계):**

1. `2>/dev/null` → `2>&1`로 변경하여 실제 에러 메시지 표시
2. KVER 감지 로직을 `/lib/modules/*-generic` 디렉토리 검색으로 변경:

```bash
# 이전 (틀림): KVER="$(uname -r)"  → WSL 커널 버전
# 이후 (맞음):
for d in /lib/modules/*-generic; do
    if [ -d "$d/kernel/drivers" ]; then
        KVER="$(basename "$d")"  # → 6.8.0-100-generic
        break
    fi
done
```

**교훈:** 에러를 숨기면(`2>/dev/null`) 디버깅이 극도로 어려워진다. 개발 중에는 항상 에러를 표시하자.

#### 문제 4: Virtio Tablet이 "상대 마우스"로 오분류

QEMU Virtio Tablet의 이벤트 비트맵:
```
B: EV=f → EV_SYN(0) + EV_KEY(1) + EV_REL(2) + EV_ABS(3)
B: REL=100  → REL_WHEEL (스크롤 휠만)
B: ABS=3    → ABS_X + ABS_Y (실제 마우스 이동)
```

- `EV_REL`과 `EV_ABS`를 **동시에** 지원!
- `EV_REL`은 스크롤 휠 전용, 실제 이동은 `EV_ABS`
- 컴포지터가 `EV_REL`을 먼저 체크 → "상대 마우스"로 분류 → `EV_ABS` 이벤트 무시 → 커서 안 움직임

**해결:** 판별 순서를 바꿈 — `EV_ABS + ABS_X`를 먼저 체크:
```c
// ABS_X 지원 여부 먼저 확인
if (TEST_BIT(EV_ABS, evbits)) {
    unsigned long absbits[NLONGS(ABS_MAX)] = {0};
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
    has_abs_x = TEST_BIT(ABS_X, absbits) != 0;
}

// 1순위: EV_ABS + ABS_X → 절대좌표 마우스 (태블릿)
// 2순위: EV_REL → 상대좌표 마우스 (일반 마우스)
// 3순위: EV_KEY + KEY_A → 키보드
```

**교훈:** 하나의 장치가 여러 이벤트 타입을 동시에 지원할 수 있다. 판별 순서가 중요하다.

#### 문제 5: Shift 키와 대문자 미지원

- `keymap_lower` 테이블 하나만 있어서 항상 소문자만 출력
- Shift 키 이벤트(`KEY_LEFTSHIFT`)가 keymap에 매핑 없어 무시됨

**해결:**

1. `keymap_upper[128]` 테이블 추가 (대문자 + 특수문자: `!@#$%^&*()`)
2. `shift_held` 전역 변수로 Shift 키 상태 추적
3. `handle_keyboard_event`에서 Shift 누름/해제를 먼저 처리
4. `keycode_to_char`에서 `shift_held`면 `keymap_upper` 사용

### VirtIO 입력 vs PS/2 입력

```
PS/2 (기본):
  i8042 칩 → atkbd (키보드) + psmouse (마우스)
  커널 내장(=y) 또는 모듈(=m)
  모듈이면 insmod 필요

VirtIO (추가):
  QEMU -device virtio-keyboard-pci -device virtio-tablet-pci
  virtio_input 커널 모듈 필요
  절대좌표 → 마우스 캡처 불필요 (마우스 통합)
```

QEMU `--gui` 모드에 VirtIO 입력 장치를 추가한 이유:

- PS/2 마우스는 `psmouse.ko` 모듈 로딩 필요 (버전 의존)
- VirtIO 태블릿은 절대좌표로 호스트↔게스트 마우스 동기화
- 마우스 캡처(grab) 없이 QEMU 창에서 자유롭게 사용 가능

### /proc/bus/input/devices 읽는 법

```
I: Bus=0006 Vendor=0627 Product=0003 Version=0002
N: Name="QEMU Virtio Tablet"
H: Handlers=mouse0 event3
B: EV=f              ← 지원하는 이벤트 타입 비트맵
B: KEY=30400 1f0000  ← 지원하는 키/버튼
B: REL=100           ← 지원하는 상대축 (bit 8 = REL_WHEEL)
B: ABS=3             ← 지원하는 절대축 (bit 0,1 = ABS_X, ABS_Y)
```

`B: EV=f` 해석: 이진수 `1111` → bit 0(EV_SYN) + bit 1(EV_KEY) + bit 2(EV_REL) + bit 3(EV_ABS)

### 수정된 파일

| 파일 | 작업 |
|------|------|
| `display/compositor/src/compositor.c` | **생성** — evdev 입력 + 윈도우 관리 + DRM 렌더링 컴포지터 |
| `display/compositor/Makefile` | **생성** — 빌드 규칙 (static 링킹) |
| `tools/mkrootfs/build-initramfs.sh` | **수정** — compositor 빌드 + 커널 모듈 복사 + KVER 수정 |
| `tools/run-qemu.sh` | **수정** — VirtIO 입력 장치 추가 |
| `kernel/config/citcos_defconfig` | **수정** — 입력 장치 CONFIG 추가 |
| `system/citcinit/services/modules.conf` | **생성** — 모듈 로딩 서비스 설정 |

### 배운 것

- Linux evdev 입력 시스템의 구조와 장치 판별 방법
- `EVIOCGBIT`, `EVIOCGABS`, `EVIOCGNAME` ioctl 사용법
- `poll()` 기반 이벤트 루프 패턴 (GUI 프로그래밍의 핵심)
- 윈도우 Z-order 관리와 Painter's Algorithm 컴포지팅
- QEMU VirtIO 입력 vs PS/2 입력의 차이
- 커널 모듈 버전 일치의 중요성 (WSL 커널 ≠ Ubuntu 커널)
- 에러 메시지를 숨기면 디버깅이 불가능해진다 (`2>/dev/null` 주의)
- 하나의 장치가 여러 이벤트 타입을 동시에 지원할 수 있다 (판별 순서 중요)
- Modifier 키(Shift) 상태를 직접 추적해야 한다 (evdev는 물리 이벤트만 전달)

---

# Class 12: CITC Display Protocol (Wayland 개념)

## 핵심 개념: 디스플레이 프로토콜

### 외부 앱이 윈도우를 만드는 원리

Class 11에서는 컴포지터 내부에서 윈도우를 하드코딩했다. 실제 데스크탑에서는 **외부 프로세스가 윈도우를 요청**한다. 이것이 Wayland 프로토콜의 핵심이다.

```
┌──────────────────┐                    ┌──────────────────┐
│ cdp_demo (클라이언트) │   Unix Socket     │ compositor (서버)  │
│                  │ ←──────────────→  │                  │
│ memfd_create()   │   /tmp/citc-      │ accept() → 연결   │
│ → mmap → 그리기   │   display-0       │ mmap → 읽기 → 합성 │
│ → commit!        │                    │ → DRM 출력        │
└──────────────────┘                    └──────────────────┘
```

### CDP ↔ Wayland 대응표

| CDP 개념 | Wayland 대응 | 역할 |
|----------|-------------|------|
| `/tmp/citc-display-0` | `$XDG_RUNTIME_DIR/wayland-0` | IPC 소켓 경로 |
| `memfd_create + SCM_RIGHTS` | `wl_shm_pool` | 공유메모리 버퍼 |
| `CDP_REQ_CREATE_SURFACE` | `wl_compositor.create_surface` | surface 생성 |
| `CDP_REQ_COMMIT` | `wl_surface.commit` | "그리기 완료!" |
| `CDP_EVT_FRAME_DONE` | `wl_callback.done` | 프레임 타이밍 |
| 포커스 surface에만 입력 전달 | `wl_keyboard.enter/leave` | 보안 입력 |

### Unix Domain Socket

같은 컴퓨터의 프로세스끼리 통신하는 소켓. TCP와 달리 네트워크를 거치지 않고 커널 내부에서 직접 데이터 전달. 파일시스템 경로를 주소로 사용.

```c
/* 서버 (compositor) */
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
bind(fd, "/tmp/citc-display-0");
listen(fd, 4);
int client = accept(fd, ...);

/* 클라이언트 (cdp_demo) */
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
connect(fd, "/tmp/citc-display-0");
```

### SCM_RIGHTS — 프로세스 간 fd 전달

Unix 소켓의 특별한 기능. `sendmsg()`의 보조 데이터(ancillary data)로 파일 디스크립터를 다른 프로세스에 전달할 수 있다.

```
클라이언트: fd=5 (memfd) → sendmsg(SCM_RIGHTS, fd=5)
          ↓ 커널이 fd가 가리키는 실제 파일 객체를 복사
서버:      fd=8 (같은 메모리) ← recvmsg()로 수신
```

이 메커니즘으로 두 프로세스가 같은 메모리를 공유!

### memfd_create — 익명 공유메모리

파일시스템에 이름이 없는 메모리 파일. fd가 닫히면 자동 정리. Wayland이 실제로 사용하는 방식.

```c
int fd = memfd_create("buffer", 0);  // 또는 syscall(SYS_memfd_create, ...)
ftruncate(fd, width * height * 4);
uint32_t *pixels = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
// pixels에 직접 그리기
// fd를 SCM_RIGHTS로 서버에 전달 → 서버도 같은 메모리 접근!
```

### 프레임 콜백 (Frame Callback)

Wayland의 중요한 렌더링 동기화 메커니즘:

```
클라이언트           서버(컴포지터)
  │                    │
  ├─ FRAME 요청 ──────→│  "다음 화면 갱신 때 알려줘"
  │                    │
  │                    ├─ render_frame() → drm_swap()
  │                    │
  │←── FRAME_DONE ─────┤  "이제 다시 그려도 됨!"
  │                    │
  ├─ render() ─────────│  새 프레임 그리기
  ├─ COMMIT ──────────→│  "그리기 완료!"
  ├─ FRAME 요청 ──────→│  다시 알려줘
  │                    │  (반복)
```

효과: 보이지 않는 앱은 FRAME을 요청하지 않음 → 전력 절약!

### 보안 입력 (X11 vs Wayland)

```
X11:     모든 앱이 다른 앱의 키 입력을 볼 수 있음 → 키로거 가능!
Wayland: 컴포지터가 포커스된 앱에만 입력 전달 → 다른 앱은 볼 수 없음
```

CDP도 동일: `comp.focused` 윈도우에 연결된 CDP surface의 클라이언트에게만 입력 이벤트를 전송.

## 문제 & 해결 (총 4개)

### 문제 1: `cdp_find_surface` 미사용 함수 에러

```
error: 'cdp_find_surface' defined but not used [-Werror=unused-function]
```

- `cdp_find_surface(surface_id)` — surface_id로 `struct cdp_surface *`를 반환하는 헬퍼
- 실제 코드에서는 `cdp_surface_index(surface_id)` (인덱스 반환)만 사용
- `-Werror`에서 경고가 에러로 승격

**해결:** 미사용 함수 삭제. 두 가지 유사한 헬퍼를 만들었으면 실제로 호출하는 쪽을 먼저 확인하자.

### 문제 2: snprintf format-truncation 경고

```
error: '%s' directive output may be truncated writing up to 251 bytes
       into a region of size 64 [-Werror=format-truncation=]
```

- CDP SET_TITLE 핸들러에서 `snprintf(win.title, 64, "%s", req->title)` 사용
- `req`는 256바이트 payload 버퍼를 캐스팅한 포인터 → 컴파일러가 `req->title`을 최대 251바이트로 판단
- 실제 `struct cdp_set_title.title`은 `char[60]`이지만, 캐스팅 때문에 컴파일러가 원래 크기를 모름

**해결:** `snprintf` 대신 `memcpy` + 명시적 null 종단으로 교체:
```c
memcpy(win.title, req->title, sizeof(win.title) - 1);
win.title[sizeof(win.title) - 1] = '\0';
```

**교훈:** 큰 버퍼를 캐스팅하면 컴파일러가 원래 구조체의 필드 크기를 알 수 없다. `snprintf("%s", ...)` 대신 `memcpy`로 바운드를 명시하면 경고를 피할 수 있다.

### 문제 3: font8x8.h include 경로 오류

```
fatal error: ../../fbdraw/src/font8x8.h: No such file or directory
```

- `cdp_demo.c`에서 `#include "../../fbdraw/src/font8x8.h"` 사용
- `display/protocol/`에서 `../..`는 프로젝트 루트의 상위 디렉토리 → 파일 없음
- 올바른 경로: `display/protocol/` → `../` → `display/` → `fbdraw/src/font8x8.h`

**해결:** `../../fbdraw/src/font8x8.h` → `../fbdraw/src/font8x8.h`

**교훈:** 상대 경로 include는 현재 파일의 위치 기준이다. 디렉토리를 한 단계씩 세어보자:
```
display/protocol/cdp_demo.c
         ↑ ../  = display/
         ↑ ../../ = (프로젝트 루트 밖!)
```

### 문제 4: font8x8 배열 이름 불일치

```
error: 'font8x8' undeclared (first use in this function)
```

- `cdp_demo.c`에서 `font8x8[(int)ch]`로 폰트 접근
- 실제 `font8x8.h`의 배열 이름은 `font8x8_basic`
- compositor.c는 자체 `font8x8` 배열을 정의했지만, 원본 헤더는 `font8x8_basic`

**해결:** `font8x8[(int)ch]` → `font8x8_basic[(int)ch]`

**교훈:** 외부 헤더를 사용할 때는 실제 심볼 이름을 반드시 확인하자. 다른 파일에서 같은 데이터를 다른 이름으로 재정의했을 수 있다.

## 수정/생성된 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `display/protocol/cdp_proto.h` | 새로 생성 | 프로토콜 정의 (메시지 타입, payload, fd 전달 헬퍼) |
| `display/protocol/cdp_client.h` | 새로 생성 | 클라이언트 라이브러리 (header-only) |
| `display/protocol/cdp_demo.c` | 새로 생성 | 데모 클라이언트 (그라디언트 + 입력 + 애니메이션) |
| `display/protocol/Makefile` | 새로 생성 | cdp_demo 빌드 |
| `display/compositor/src/compositor.c` | 수정 | CDP 서버 추가 (소켓, 클라이언트 관리, SHM 렌더링) |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | cdp_demo 빌드 단계 추가 (3.8단계) |

## 사용법

```bash
# QEMU --gui 모드에서 부팅 후, 시리얼 콘솔에서:
compositor &          # 컴포지터 백그라운드 실행
sleep 2               # 소켓 생성 대기
cdp_demo              # CDP 클라이언트 실행!
```

### 주요 학습 포인트

- Unix domain socket으로 프로세스 간 통신
- memfd_create + SCM_RIGHTS로 제로카피 버퍼 공유
- 메시지 기반 프로토콜 (헤더 + payload 패턴)
- 프레임 콜백으로 렌더링 타이밍 동기화
- poll() 루프에 여러 종류의 fd 통합 (evdev + 소켓)
- 컴포지터의 역할 변화: "그리기 도구" → "디스플레이 서버"

---

## Class 13: PE 로더 (Windows .exe 실행)

**파일:** `wcl/include/pe.h`, `wcl/src/loader/citcrun.c`

### 핵심 개념: PE (Portable Executable) 포맷

#### Windows .exe의 구조

Windows 실행파일(.exe, .dll)은 **PE 포맷**을 사용한다. 1993년 Windows NT에서 도입되었으며, 원래 VAX/VMS의 COFF 포맷에서 파생되었다.

```
.exe 파일 레이아웃:

┌─────────────────────┐  오프셋 0
│  DOS Header (64B)   │  ← "MZ" 매직 (1987년 Mark Zbikowski의 이니셜)
│  e_lfanew → ──────────→ PE 헤더 오프셋 (보통 0x80~0x100)
├─────────────────────┤
│  DOS Stub           │  ← "This program cannot be run in DOS mode."
├─────────────────────┤  e_lfanew 오프셋
│  PE Signature (4B)  │  ← "PE\0\0" (0x00004550)
│  File Header (20B)  │  ← Machine, NumberOfSections, Characteristics
│  Optional Hdr (240B)│  ← ImageBase, EntryPoint, SizeOfImage
│    DataDirectory[16]│  ← Import Table, Relocation Table 등의 위치
├─────────────────────┤
│  Section Headers    │  ← 각 40바이트, 섹션 이름/주소/크기/속성
├─────────────────────┤
│  .text              │  ← 실행 코드 (R-X)
│  .rdata             │  ← 읽기 전용 데이터, Import Table (R--)
│  .data              │  ← 읽기/쓰기 데이터 (RW-)
│  .reloc             │  ← 베이스 재배치 테이블 (R--)
└─────────────────────┘
```

#### PE vs ELF 비교

| 개념 | PE (Windows) | ELF (Linux) | 배우는 것 |
|------|-------------|-------------|----------|
| 매직 넘버 | `"MZ"` + `"PE\0\0"` | `"\x7FELF"` | 파일 형식 식별 |
| 엔트리포인트 | `OptionalHeader.AddressOfEntryPoint` | `e_entry` | 실행 시작 주소 |
| 섹션 | `.text`, `.rdata`, `.reloc` | `.text`, `.rodata`, `.data` | 코드/데이터 분리 |
| 동적 링킹 | Import Table (ILT → IAT) | GOT/PLT | 외부 함수 연결 |
| 주소 독립성 | Base Relocation Table | PIC (위치 독립 코드) | 주소 고정 방법 |
| 호출 규약 | Microsoft x64 (rcx,rdx,r8,r9) | System V (rdi,rsi,rdx,rcx) | ABI 차이 |

#### DOS 헤더의 역사

PE 파일이 왜 "MZ"로 시작할까?

1. **1981년:** MS-DOS 실행파일 포맷 탄생. 매직 넘버 "MZ" (Mark Zbikowski, MS-DOS 개발자)
2. **1987년:** Windows 1.0 — NE(New Executable) 포맷. DOS 헤더 뒤에 NE 헤더 추가
3. **1993년:** Windows NT — PE 포맷 도입. 여전히 DOS 헤더를 앞에 유지

DOS 헤더를 유지하는 이유: **하위 호환성**. DOS에서 PE 파일을 실행하면 DOS 스텁이 "This program cannot be run in DOS mode" 메시지를 출력하고 종료한다. `e_lfanew` 필드가 실제 PE 헤더의 오프셋을 가리킨다.

### 핵심 개념: PE 로딩 파이프라인

Windows 로더(ntdll.dll의 LdrLoadDll)가 .exe를 실행하는 과정을 우리가 직접 구현한다:

```
┌────────────────────────────────────────────────────────────────┐
│                    PE 로딩 파이프라인                            │
│                                                                │
│  1. DOS 헤더  ─→  "MZ" 확인, e_lfanew로 PE 헤더 찾기            │
│  2. PE 헤더   ─→  시그니처/Machine/Magic 검증                    │
│  3. 섹션 읽기  ─→  .text, .rdata, .reloc 등 헤더 파싱            │
│  4. 메모리 매핑 ─→  mmap으로 SizeOfImage만큼 예약 + 섹션별 매핑    │
│  5. 리로케이션 ─→  실제 로드 주소 ≠ ImageBase일 때 주소 보정       │
│  6. 임포트 해석 ─→  IAT에 스텁 함수 주소 기록                     │
│  7. 엔트리포인트 ─→  AddressOfEntryPoint로 점프!                  │
└────────────────────────────────────────────────────────────────┘
```

### 핵심 개념: 섹션 매핑 (mmap)

PE 파일의 섹션을 메모리에 올리는 3단계:

```c
// 1단계: 전체 주소 공간 예약 (아직 접근 불가)
base = mmap(NULL, SizeOfImage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

// 2단계: 각 섹션을 정확한 위치에 매핑 + 파일 데이터 복사
for (각 섹션) {
    addr = base + section.VirtualAddress;
    mmap(addr, size, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS);
    pread(fd, addr, section.SizeOfRawData, section.PointerToRawData);
}

// 3단계: 리로케이션/임포트 처리 후 최종 보호 속성 적용
mprotect(.text, PROT_READ | PROT_EXEC);   // 코드: 실행 가능
mprotect(.rdata, PROT_READ);               // 읽기 전용 데이터
```

**왜 2단계로?** 리로케이션과 임포트 해석 시 메모리에 쓰기가 필요하므로, 먼저 RW로 매핑한 뒤 나중에 최종 권한을 설정한다.

### 핵심 개념: 베이스 리로케이션

PE 파일은 "선호 로드 주소" (`ImageBase`, 보통 `0x140000000`)를 가진다. 하지만 실제로는 ASLR이나 다른 이유로 다른 주소에 로드될 수 있다.

```
ImageBase (선호):     0x0000000140000000
실제 로드 주소:       0x00007f1234560000
delta (차이):         실제 - 선호 = 0x00007f10F4560000

.reloc 섹션의 각 엔트리:
  "0x1234 주소에 있는 64비트 값에 delta를 더하세요"
  → *(uint64_t*)(base + 0x1234) += delta;
```

ELF는 PIC(Position Independent Code)를 사용해서 리로케이션이 불필요한 경우가 많지만, PE는 절대 주소를 하드코딩하므로 반드시 보정이 필요하다.

### 핵심 개념: 임포트 해석 (Import Table)

Windows 프로그램은 DLL의 함수를 호출한다 (예: `kernel32.dll::WriteFile`). PE 로더는 이 연결을 만들어야 한다:

```
.exe의 Import Table 구조:

IMAGE_IMPORT_DESCRIPTOR (DLL마다 1개)
├── Name RVA ────────→ "kernel32.dll"
├── OriginalFirstThunk ──→ ILT (Import Lookup Table) [읽기 전용 원본]
│   ├── → IMAGE_IMPORT_BY_NAME { "ExitProcess" }
│   ├── → IMAGE_IMPORT_BY_NAME { "GetStdHandle" }
│   └── → IMAGE_IMPORT_BY_NAME { "WriteFile" }
└── FirstThunk ──────────→ IAT (Import Address Table) [여기에 주소 기록!]
    ├── → stub_ExitProcess의 실제 주소
    ├── → stub_GetStdHandle의 실제 주소
    └── → stub_WriteFile의 실제 주소

프로그램이 WriteFile()를 호출하면:
  call [IAT + offset]  →  우리가 넣어둔 stub_WriteFile() 실행!
```

### 핵심 개념: 호출 규약 (Calling Convention) 차이

Windows .exe는 **Microsoft x64 ABI**로 컴파일된다. 우리의 스텁 함수는 Linux에서 실행되므로, GCC에게 Windows 규약을 사용하라고 알려줘야 한다:

```
Microsoft x64 ABI          System V x64 ABI (Linux)
─────────────────          ─────────────────────────
1번째 인수: RCX             1번째 인수: RDI
2번째 인수: RDX             2번째 인수: RSI
3번째 인수: R8              3번째 인수: RDX
4번째 인수: R9              4번째 인수: RCX
Shadow space: 32바이트      Red zone: 128바이트
```

```c
// __attribute__((ms_abi))로 Windows 호출 규약 사용
__attribute__((ms_abi))
static int32_t stub_WriteFile(void *handle, const void *buf,
                              uint32_t bytes, uint32_t *written,
                              void *overlapped)
{
    // Windows .exe가 RCX, RDX, R8, R9로 인수를 전달하면
    // GCC가 자동으로 올바르게 받아준다
    ssize_t ret = write((int)(intptr_t)handle, buf, bytes);
    if (written) *written = (uint32_t)ret;
    return (ret >= 0) ? 1 : 0;
}
```

### 핵심 개념: Wine의 원리

이 PE 로더가 바로 **Wine의 핵심 원리**이다:

```
Wine (실제)                  citcrun (우리 구현)
──────────                   ──────────────────
PE 파싱 + 메모리 매핑         ← 동일
ntdll.dll 에뮬레이션          ← 미구현 (향후)
kernel32.dll 함수 1000+개     ← 3개 스텁 (ExitProcess, GetStdHandle, WriteFile)
user32/gdi32 GUI              ← 미구현 (향후)
DirectX → Vulkan (DXVK)      ← 미구현 (향후)
레지스트리, COM, .NET          ← 미구현 (향후)
```

Wine은 이 과정을 30년간 발전시켜 수천 개의 Windows API를 구현했다. 우리는 같은 원리의 최소 버전을 만들었다.

### 구현 상세

#### 스텁 함수 (3개)

| Windows API | 스텁 구현 | 역할 |
|-------------|----------|------|
| `ExitProcess(code)` | `_exit(code)` | 프로세스 종료 |
| `GetStdHandle(id)` | fd 0/1/2 반환 | 표준 입출력 핸들 |
| `WriteFile(h,buf,n,...)` | `write(fd,buf,n)` | 파일/콘솔 쓰기 |

#### 테스트 프로그램 (hello_win.c)

```c
// CRT 없이 _start에서 직접 시작 (msvcrt.dll 불필요!)
// x86_64-w64-mingw32-gcc -nostdlib -o hello.exe hello_win.c -lkernel32
void _start(void) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(h, "Hello from Windows .exe on CITC OS!\n", 36, &written, 0);
    ExitProcess(0);
}
```

`-nostdlib` 플래그로 C 런타임을 제외하면, `msvcrt.dll`의 수백 개 함수를 구현할 필요가 없다. 오직 `kernel32.dll`의 3개 함수만 있으면 실행 가능.

### 문제 & 해결 (총 2개)

#### 문제 1: `.reloc` 섹션 없는 PE에서 로더 중단

```
  경고: .reloc 섹션 없음 (리로케이션 불가)
[4/5] 리로케이션 적용.../
(여기서 멈춤)
```

- MinGW `-nostdlib`로 빌드한 .exe에는 `.reloc` 섹션이 생성되지 않음
- `pe_apply_relocations()`가 `.reloc` 없으면 `-1` 반환 → 프로그램 종료
- 또한 `printf`가 `\n` 없이 출력 + `fprintf(stderr, ...)`와 섞여서 출력 순서 꼬임

**해결:** `.reloc` 없을 때 실패 대신 경고만 출력하고 `return 0`. x86_64 PE는 RIP-relative 주소를 사용하므로 리로케이션 없이도 동작할 수 있다.

```c
// Before: return -1 (fatal)
// After:
printf("  .reloc 없음 — RIP-relative 코드로 간주 (skip)\n");
return 0;
```

**교훈:** x86_64에서는 PE도 RIP-relative 주소 지정을 사용하는 경우가 많다. `.reloc` 섹션은 필수가 아니라 선택적이다. 32비트 PE와 달리 64비트 PE는 리로케이션 없이 동작 가능한 경우가 많음.

#### 문제 2: Segmentation fault — NX bit 보호

```text
>>> 엔트리포인트 실행 (RVA=0x1000) >>>
Segmentation fault
```

- 섹션을 `mmap(PROT_READ | PROT_WRITE)`로 매핑 (리로케이션/임포트 패치를 위해)
- 패치 완료 후 **최종 보호 속성을 적용하지 않음**
- `.text` 섹션에 `PROT_EXEC`(실행 권한)가 없음
- 현대 CPU의 NX bit(No-Execute)가 비실행 메모리에서의 코드 실행을 차단 → segfault

**해결:** `pe_set_section_protection()` 함수 추가. 임포트 해석 후, 엔트리포인트 호출 전에 `mprotect()`로 PE 헤더의 Characteristics에 맞게 권한 설정:
```c
// .text  → PROT_READ | PROT_EXEC  (코드 실행 가능)
// .rdata → PROT_READ              (읽기 전용)
// .idata → PROT_READ              (IAT 패치 완료 후 잠금)
mprotect(base + s->VirtualAddress, size, prot);
```

**교훈:** OS 로더가 하는 일은 "매핑 → 패치 → 보호 → 실행" 4단계이다. 3단계(보호)를 빠뜨리면 NX bit가 실행을 차단한다. Linux의 ELF 로더도 `load_elf_binary()`에서 같은 순서를 따른다.

### 수정/생성된 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `wcl/include/pe.h` | 새로 생성 | PE 포맷 구조체 정의 (DOS/PE/섹션/임포트/리로케이션) |
| `wcl/src/loader/citcrun.c` | 새로 생성 | PE 로더 (파싱→매핑→리로케이션→임포트→실행) |
| `wcl/src/loader/Makefile` | 새로 생성 | citcrun 빌드 (gcc -static) |
| `wcl/tests/hello_win.c` | 새로 생성 | 테스트 Windows 프로그램 (CRT 없음) |
| `wcl/tests/Makefile` | 새로 생성 | MinGW 크로스 컴파일 |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | citcrun + hello.exe 빌드 단계 추가 (3.10, 3.11) |

### 사용법

```bash
# QEMU 부팅 후:
citcrun /opt/wcl-tests/hello.exe            # 로드 + 실행
citcrun --info /opt/wcl-tests/hello.exe     # PE 헤더 덤프만

# 기대 출력:
# === CITC PE Loader ===
# [1/5] DOS 헤더 읽기... MZ ✓
# [2/5] PE 헤더 읽기... PE32+ (x86_64) ✓
# [3/5] 섹션 매핑 (3개)...
# [4/5] 리로케이션 적용...
# [5/5] 임포트 해석...
# >>> 엔트리포인트 실행 >>>
# Hello from Windows .exe on CITC OS!
# >>> 프로세스 종료 (코드: 0) <<<
```

### 주요 학습 포인트

- PE 포맷의 계층 구조 (DOS → PE → 섹션 → 데이터 디렉토리)
- mmap으로 바이너리를 메모리에 매핑하는 방법 (MAP_FIXED, pread)
- 베이스 리로케이션으로 절대 주소를 보정하는 원리
- Import Table(ILT/IAT)로 DLL 함수를 연결하는 메커니즘
- Windows vs Linux 호출 규약 차이와 `__attribute__((ms_abi))`
- Wine의 핵심 원리: PE 파싱 + API 번역

---

## Class 14: 터미널 에뮬레이터 (PTY + CDP)

**파일:** `display/terminal/src/citcterm.c`

### 핵심 개념: PTY (Pseudo-Terminal)

#### 터미널이란?

원래 "터미널"은 물리적 장치였다. 1978년 DEC VT100 — 키보드와 모니터가 달린 장비로, 시리얼 케이블로 메인프레임에 연결. 사용자가 키보드를 치면 시리얼로 문자가 전송되고, 메인프레임이 보낸 문자가 화면에 표시됨.

현대 "터미널 에뮬레이터"(gnome-terminal, xterm, iTerm2)는 이 물리 장치를 소프트웨어로 구현한 것. **PTY(Pseudo-Terminal)**가 그 핵심이다.

#### PTY의 구조

```
                    커널
┌─────────────────────────────────────────┐
│                                         │
│  PTY Master ←──────→ PTY Slave          │
│  (fd)              (/dev/pts/0)         │
│                                         │
│  Line Discipline:                       │
│    - echo (입력을 화면에 표시)             │
│    - Ctrl+C → SIGINT                    │
│    - \r → \r\n 변환                     │
│    - canonical mode (줄 단위 편집)        │
│                                         │
└──────┬──────────────────────┬───────────┘
       │                      │
  citcterm                  /bin/sh
  (터미널 에뮬레이터)        (쉘 프로세스)
  - master에 쓰기            - slave에서 읽기 (stdin)
    → 쉘이 읽음              - slave에 쓰기 (stdout)
  - master에서 읽기            → 에뮬레이터가 읽음
    → 화면에 표시
```

#### PTY 생성 과정 (POSIX API)

```c
// 1. 마스터 열기 → fd 반환
int master = posix_openpt(O_RDWR | O_NOCTTY);

// 2. 슬레이브 권한 설정 + 잠금 해제
grantpt(master);
unlockpt(master);

// 3. 슬레이브 경로 얻기
char *slave_name = ptsname(master);  // → "/dev/pts/0"

// 4. 윈도우 크기 설정 (쉘이 터미널 크기를 알아야 함)
struct winsize ws = { .ws_row = 25, .ws_col = 80 };
ioctl(master, TIOCSWINSZ, &ws);

// 5. fork()
pid_t pid = fork();
if (pid == 0) {
    // 자식: setsid() → 새 세션 리더
    setsid();
    // 슬레이브를 stdin/stdout/stderr로
    int slave = open(slave_name, O_RDWR);
    dup2(slave, 0); dup2(slave, 1); dup2(slave, 2);
    // 쉘 실행
    execl("/bin/sh", "sh", NULL);
}
```

#### setsid()가 필요한 이유

`fork()`된 자식은 부모의 세션에 속한다. `setsid()`로 새 세션을 만들어야:
1. 새 세션 + 새 프로세스 그룹의 리더가 됨
2. 제어 터미널이 없는 상태가 됨
3. PTY 슬레이브를 열면 그것이 제어 터미널이 됨

이것이 Ctrl+C(SIGINT)가 작동하는 핵심: 제어 터미널에서 인터럽트 → 포그라운드 프로세스 그룹에 SIGINT 전달.

### 핵심 개념: ANSI 이스케이프 시퀀스

쉘이 출력하는 데이터에는 텍스트와 제어 코드가 섞여 있다:

```
"Hello\033[2J\033[H"
 ─────              → 텍스트 "Hello"
      ───────       → ESC[2J = 화면 전체 지우기
             ───── → ESC[H  = 커서 홈(0,0)
```

#### 상태 기계로 파싱

```
ESC_NORMAL ──(0x1B)──→ ESC_ESC ──('[')──→ ESC_CSI
    ↑                     │                    │
    │                     │ (기타: 무시)        │ (숫자/';': 수집)
    │                     ↓                    │ (알파벳: 실행!)
    └─────────────────────┘                    │
    └──────────────────────────────────────────┘
```

#### 지원하는 CSI 명령

| 시퀀스 | 이름 | 동작 |
|--------|------|------|
| `ESC[nA` | CUU | 커서 위로 n칸 |
| `ESC[nB` | CUD | 커서 아래로 n칸 |
| `ESC[nC` | CUF | 커서 오른쪽 n칸 |
| `ESC[nD` | CUB | 커서 왼쪽 n칸 |
| `ESC[row;colH` | CUP | 커서 위치 지정 |
| `ESC[J` / `ESC[2J` | ED | 화면 지우기 |
| `ESC[K` | EL | 줄 끝까지 지우기 |
| `ESC[...m` | SGR | 색상/속성 (v1에서는 무시) |

### 핵심 개념: poll() 멀티플렉싱

터미널은 **두 가지 이벤트 소스**를 동시에 감시해야 한다:

```c
struct pollfd fds[2];
fds[0].fd = conn->sock_fd;   // CDP 소켓 (키보드 이벤트)
fds[1].fd = pty_master;       // PTY (쉘 출력)

while (running) {
    poll(fds, 2, 100);

    if (fds[0] ready)  // 키보드 → PTY에 전달
        cdp_recv_msg() → term_handle_key() → write(pty_master)

    if (fds[1] ready)  // 쉘 출력 → 화면에 표시
        read(pty_master) → term_putchar() → 렌더링

    if (dirty)  // 변경사항이 있으면 화면 갱신
        term_render() → cdp_commit_to()
}
```

**cdp_dispatch()를 사용하지 않는 이유:** `cdp_dispatch()`는 내부에서 `cdp_recv_msg()`를 블로킹 호출. PTY에서 데이터가 와도 처리 불가. → `poll()`로 직접 멀티플렉싱.

### 핵심 개념: 키보드 입력 → PTY 전달

| 키 | PTY에 쓰는 값 | 쉘이 하는 일 |
|----|--------------|-------------|
| 일반 문자 | 그 문자 | 입력 버퍼에 추가 |
| Enter | `'\r'` | 명령 실행 |
| Backspace | `'\x7f'` (DEL) | 마지막 글자 삭제 |
| Tab | `'\t'` | 자동 완성 |
| ↑/↓/→/← | `"\033[A/B/C/D"` | 히스토리/커서 이동 |
| Ctrl+C | `'\x03'` | 포그라운드 프로세스에 SIGINT |
| Ctrl+D | `'\x04'` | EOF (쉘 종료) |

### 구현 상세

#### 컴포지터 수정: Ctrl 키 지원

```c
// keycode_to_char()에 Ctrl 처리 추가
if (ctrl_held) {
    char base = keymap_lower[code];
    if (base >= 'a' && base <= 'z')
        return base - 'a' + 1;  // Ctrl+C → 0x03
}
```

### 수정/생성된 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `display/terminal/src/citcterm.c` | 새로 생성 | 터미널 에뮬레이터 (PTY + ANSI 파서 + CDP 렌더링) |
| `display/terminal/Makefile` | 새로 생성 | citcterm 빌드 (gcc -static) |
| `display/compositor/src/compositor.c` | 수정 | Ctrl 키 지원 추가 (ctrl_held + 제어 문자 변환) |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | citcterm 빌드 단계 추가 (3.85) |

### 사용법

```bash
# QEMU --gui 모드에서 부팅 후, 시리얼 콘솔에서:
compositor &
sleep 2
citcterm              # 터미널 윈도우가 화면에 나타남
# 윈도우를 클릭하여 포커스 → 쉘 사용 가능!
```

### 주요 학습 포인트

- PTY(Pseudo-Terminal)의 master/slave 구조와 Line Discipline
- posix_openpt() + grantpt() + unlockpt() + ptsname() PTY 생성 API
- setsid()로 세션 리더 설정 → 제어 터미널 연결의 전제조건
- ANSI 이스케이프 시퀀스를 상태 기계로 파싱하는 방법
- poll()로 여러 fd를 동시에 감시하는 이벤트 루프 설계
- 키보드 입력 → PTY → 쉘 → PTY → 화면의 전체 데이터 흐름
- Ctrl 키와 제어 문자(ASCII 1-26)의 관계

### 문제 & 해결 (총 3개)

#### 1. Makefile 헤더 경로 vs 소스 파일 #include 경로

```
make: *** No rule to make target '../../protocol/cdp_proto.h', needed by 'build/citcterm'.
```

**원인:**
Makefile의 HEADERS 의존성과 소스 파일의 `#include`는 **기준 디렉토리가 다르다:**

```
display/terminal/           ← Makefile이 여기서 실행됨
display/terminal/src/       ← 소스 파일이 여기 있음
display/protocol/           ← 헤더가 여기 있음
```

| 컨텍스트 | 기준 디렉토리 | 올바른 경로 |
|---------|-------------|-----------|
| Makefile HEADERS | `display/terminal/` (make 실행 위치) | `../protocol/cdp_proto.h` |
| `#include ""` | `display/terminal/src/` (소스 파일 위치) | `../../protocol/cdp_client.h` |

make는 Makefile 기준, gcc는 소스 파일 기준으로 상대 경로를 해석한다.

**해결:** Makefile의 HEADERS를 `../protocol/`로 수정 (소스의 `#include`는 원래 맞았음).

#### 2. Feature Test Macro 충돌 — _XOPEN_SOURCE vs _GNU_SOURCE

```
error: implicit declaration of function 'posix_openpt'
error: implicit declaration of function 'grantpt'
error: implicit declaration of function 'unlockpt'
error: implicit declaration of function 'ptsname'
```

**1차 시도:** `#define _XOPEN_SOURCE 600` 추가 → PTY 함수는 해결됐지만:

```
error: implicit declaration of function 'syscall'   ← cdp_client.h의 memfd_create
```

**원인:** Feature Test Macro의 계층 구조:

```
_GNU_SOURCE (가장 넓음)
  └── _XOPEN_SOURCE 700
       └── _XOPEN_SOURCE 600  ← PTY 함수 (posix_openpt 등)
  └── Linux 확장              ← syscall(), memfd_create 등

_XOPEN_SOURCE만 정의하면 Linux 확장이 숨겨진다!
```

**해결:** `#define _GNU_SOURCE` — POSIX와 Linux 확장을 모두 포함.

#### 3. -Werror로 인한 경고 → 에러 변환

```
error: 'cdp_dispatch' defined but not used [-Werror=unused-function]
error: ignoring return value of 'write' [-Werror=unused-result]
```

| 경고 | 원인 | 해결 |
|------|------|------|
| `cdp_dispatch` 미사용 | header-only 라이브러리의 static 함수. citcterm은 자체 poll() 루프 사용 | `#pragma GCC diagnostic ignored "-Wunused-function"` |
| `write()` 반환값 무시 | glibc의 `__wur` 속성이 반환값 체크 강제 | `ssize_t ret = write(...); (void)ret;` |

**교훈:** `-Werror`는 좋은 습관이지만, header-only 라이브러리를 포함할 때는 pragma로 특정 경고를 선택적으로 억제해야 할 수 있다.

---

## Class 15: 커스텀 쉘 (citcsh)

> **목표:** bash, zsh 같은 쉘의 원리를 직접 구현 — 토크나이저, 파이프, 리다이렉션, fork+exec

### 쉘이란?

쉘(shell)은 사용자와 OS 커널 사이의 인터페이스이다. 이름의 유래: 커널(kernel=핵심)을 감싸는 "껍질(shell)"이라는 의미.

```
사용자 → 쉘 → 커널 → 하드웨어
         ↑
    "ls | grep foo"를
    fork+exec+pipe로 변환하는 통역사
```

쉘은 **단순한 프로그램**이다. 특별한 커널 권한이 없다. fork(), exec(), pipe(), dup2() 같은 일반 시스템 콜만 사용한다.

### 핵심 아키텍처: 3단계 파이프라인

```
입력: "ls /bin | grep cit | wc -l"
          ↓
  ┌──────────────┐
  │ 1. 토크나이저 │ → [WORD:"ls"] [WORD:"/bin"] [PIPE] [WORD:"grep"] ...
  └──────┬───────┘
         ↓
  ┌──────────────┐
  │ 2. 파서      │ → Pipeline{ cmd[0]={"ls","/bin"}, cmd[1]={"grep","cit"}, cmd[2]={"wc","-l"} }
  └──────┬───────┘
         ↓
  ┌──────────────┐      pipe()          pipe()
  │ 3. 실행 엔진 │  ls =======> grep =======> wc → stdout
  └──────────────┘  fork+exec  fork+exec  fork+exec
```

이 3단계 구조는 컴파일러와 동일한 패턴이다:
- 렉서(토크나이저) → 파서 → 코드 생성(실행)

### fork() + exec() — UNIX 프로세스 생성의 핵심

```c
pid = fork();        /* 현재 프로세스의 복제본 생성 */
if (pid == 0) {
    /* 자식: 복제된 프로세스 */
    execvp("grep", argv);  /* 코드를 grep으로 교체 */
    /* exec 성공시 여기에 절대 도달 안 함! */
}
/* 부모: 원래 쉘 프로세스 */
waitpid(pid, &status, 0);  /* 자식 종료 대기 */
```

왜 fork()와 exec()이 분리되어 있는가?

**fork()와 exec() 사이**에 파이프/리다이렉션을 설정할 수 있기 때문이다:

```c
pid = fork();
if (pid == 0) {
    /* 여기서 파이프, 리다이렉션 설정! */
    dup2(pipe_fd, STDOUT_FILENO);   /* stdout → 파이프 */
    close(pipe_fd);
    execvp("ls", argv);   /* ls는 stdout이 파이프인 줄 모름 */
}
```

Windows의 `CreateProcess()`는 이 유연성이 없다 — 프로세스 생성과 프로그램 실행이 하나로 묶여있다.

### pipe() — 프로세스 간 데이터 채널

```c
int fds[2];
pipe(fds);    /* fds[0]=읽기 끝, fds[1]=쓰기 끝 */
```

pipe()가 만드는 것은 **커널 내부의 4KB 버퍼**이다:

```
프로세스 A → write(fds[1]) → [커널 버퍼 4KB] → read(fds[0]) → 프로세스 B
```

`ls | grep foo` 실행 과정:

```
1. pipe() → fds[0], fds[1]
2. fork() → 자식1 (ls):   dup2(fds[1], STDOUT)  → ls의 출력이 파이프로
3. fork() → 자식2 (grep): dup2(fds[0], STDIN)   → grep의 입력이 파이프에서
4. 부모: close(fds[0]), close(fds[1])  → 부모는 파이프 안 씀
5. 부모: waitpid() × 2
```

N개 명령 파이프라인 = N-1개 pipe() + N번 fork():

```
cmd0 → pipe[0] → cmd1 → pipe[1] → cmd2 → ... → cmdN-1
       [0][1]          [1][1]
```

### dup2() — 리다이렉션의 원리

```c
int fd = open("output.txt", O_WRONLY | O_CREAT, 0644);
dup2(fd, STDOUT_FILENO);   /* fd가 가리키는 곳을 stdout도 가리키게 */
close(fd);
/* 이제 printf()가 output.txt에 쓴다! */
```

`echo hello > out.txt`의 실제 동작:

```
쉘: fork()
자식: open("out.txt") → dup2(fd, 1) → exec("echo", "hello")
      echo는 자기가 stdout에 쓰는 줄 알지만, 실제로는 파일에 쓴다!
```

### 토크나이저 (렉서)

입력 문자열을 토큰으로 분리하는 과정:

```
입력: ls -l "hello world" | grep foo > out.txt &

토큰: [WORD:"ls"] [WORD:"-l"] [WORD:"hello world"]
      [PIPE] [WORD:"grep"] [WORD:"foo"]
      [REDIR_OUT] [WORD:"out.txt"]
      [BACKGROUND]
```

따옴표의 역할:
- `echo hello world` → 인자 2개: "hello", "world"
- `echo "hello world"` → 인자 1개: "hello world"
- 따옴표 안에서 공백은 토큰 구분자가 아니다

### 빌트인 명령이 필요한 이유

`cd`를 fork+exec으로 실행하면?

```
부모(쉘): cwd = /root
  └── fork()
      └── 자식: chdir("/tmp")  ← 자식의 cwd만 변경!
                자식 종료
부모(쉘): cwd = /root  ← 안 바뀜!!
```

cd는 **쉘 자체의 상태**를 변경해야 하므로, fork 없이 쉘 프로세스에서 직접 `chdir()`를 호출해야 한다. 같은 이유로 `exit`, `export`도 빌트인이다.

### 시그널 처리

```
Ctrl+C 눌렀을 때:
  터미널 드라이버 → SIGINT → 포그라운드 프로세스 그룹

쉘의 규칙:
  쉘 자체: SIGINT 무시 (SIG_IGN) — 죽지 않음!
  자식(실행 중인 명령): SIGINT 기본 (SIG_DFL) — Ctrl+C로 종료됨

구현:
  setup_signals(): sigaction(SIGINT, SIG_IGN)  /* 쉘 보호 */
  fork() 후 자식에서: signal(SIGINT, SIG_DFL)  /* 자식은 종료 가능 */
```

SIGCHLD와 좀비 프로세스:
```
좀비(zombie) = 종료됐지만 부모가 waitpid()로 수거하지 않은 자식
  → 프로세스 테이블에 항목이 남아있음 (리소스 누수)

해결: SIGCHLD 핸들러에서 waitpid(-1, NULL, WNOHANG) 루프
  → 백그라운드 프로세스 종료 즉시 수거
```

### 환경변수 확장

```bash
export MY_VAR=citcos
echo $MY_VAR          # → "citcos"
echo $HOME            # → "/root"
echo $?               # → 마지막 명령의 종료 코드
```

쉘이 `echo $HOME`을 실행할 때:
1. `$HOME` → `getenv("HOME")` → `"/root"` 치환
2. `echo`에 `"/root"`를 인자로 전달
3. echo는 $HOME이 뭔지 전혀 모른다 — 이미 치환된 값만 받는다

### 구현 파일

| 파일 | 역할 |
|------|------|
| `system/citcsh/src/citcsh.c` | 새로 생성 — 커스텀 쉘 (~700줄) |
| `system/citcsh/Makefile` | 새로 생성 — 빌드 설정 |
| `system/citcinit/src/main.c` | 수정 — 쉘 목록에 citcsh 추가 |
| `display/terminal/src/citcterm.c` | 수정 — citcsh 우선 실행 |
| `tools/mkrootfs/build-initramfs.sh` | 수정 — citcsh 빌드 단계 추가 |

### 사용법

```bash
# 빌드 후 QEMU 부팅
compositor &
sleep 2
citcterm          # citcsh 자동 실행

# citcsh에서:
/ # ls /bin | grep cit
citcrun
citcsh
citcterm
/ # echo hello > /tmp/test.txt
/ # cat /tmp/test.txt
hello
/ # export MY_VAR=os
/ # echo $MY_VAR
os
/ # help              # 빌트인 목록
/ # exit
```

### 문제 & 해결 (총 2개)

#### 문제 1: 빌트인 명령에서 리다이렉션이 작동하지 않음

**증상:** `echo hello > /tmp/out.txt` 실행 시 파일이 생성되지 않고 화면에 출력됨.

**원인:** 빌트인 명령(echo, cd, export 등)은 성능을 위해 fork 없이 부모 프로세스에서 직접 실행하는 "빠른 경로"를 탔는데, 이 경로에서 `setup_redirections()`를 호출하지 않았다.

```
일반 명령:  fork() → setup_redirections() → execvp()  ← 리다이렉션 정상
빌트인:     run_builtin() 직접 호출                     ← 리다이렉션 누락!
```

**해결:** 빌트인 빠른 경로에서도 리다이렉션을 적용하되, 부모 프로세스의 stdin/stdout/stderr을 보존해야 한다. `dup()`으로 원본 fd를 저장하고, 빌트인 실행 후 `dup2()`로 복원:

```c
/* 빌트인 빠른 경로 — 리다이렉션 지원 */
int saved_in = dup(STDIN_FILENO);    /* 원본 저장 */
int saved_out = dup(STDOUT_FILENO);
int saved_err = dup(STDERR_FILENO);

setup_redirections(cmd);              /* stdout → 파일 등 */
run_builtin(cmd->argv);              /* 빌트인 실행 */
fflush(stdout);

dup2(saved_in, STDIN_FILENO);        /* 원본 복원 */
dup2(saved_out, STDOUT_FILENO);
dup2(saved_err, STDERR_FILENO);
close(saved_in); close(saved_out); close(saved_err);
```

**교훈:** fork()를 하는 일반 명령은 자식 프로세스가 종료되면 fd 변경이 사라지지만, 빌트인은 부모에서 실행되므로 fd를 직접 보존/복원해야 한다.

#### 문제 2: exit 실행 시 컴포지터가 함께 종료됨

**증상:** citcsh에서 `exit` 입력 → 쉘만 종료되어야 하는데 컴포지터 전체가 죽음.

**원인:** SIGPIPE 미처리. citcsh가 exit하면 citcterm도 종료되고, CDP 소켓이 닫힌다. 컴포지터가 닫힌 소켓에 `write()`하면 SIGPIPE 시그널이 발생하는데, SIGPIPE의 기본 동작은 **프로세스 종료**다.

```
citcsh exit → citcterm 종료 → CDP 소켓 닫힘
                                    ↓
컴포지터: write(closed_socket, ...) → SIGPIPE → 기본 동작: 프로세스 종료!
```

**해결:** 컴포지터 main() 시작부에 SIGPIPE 무시 설정:

```c
#include <signal.h>
signal(SIGPIPE, SIG_IGN);  /* 클라이언트 연결 끊김에도 생존 */
```

이렇게 하면 `write()`가 SIGPIPE 대신 `-1`을 반환하고 `errno = EPIPE`가 설정되어, 에러 처리 코드에서 정상적으로 클라이언트를 제거할 수 있다.

**교훈:** 서버 프로세스는 반드시 `signal(SIGPIPE, SIG_IGN)`을 해야 한다. 클라이언트가 언제든 연결을 끊을 수 있고, 그때마다 서버가 죽으면 안 된다. nginx, Apache 등 모든 서버가 이 패턴을 사용한다.

### 주요 학습 포인트

- 쉘의 3단계 구조: 토크나이저 → 파서 → 실행 엔진
- fork() + exec()의 분리 설계 — UNIX의 핵심 설계 철학
- pipe()의 커널 버퍼 구조와 N개 명령 파이프라인 알고리즘
- dup2()를 이용한 파일 디스크립터 리다이렉션의 원리
- 빌트인 명령이 필요한 이유 (cd, exit, export)
- SIGINT/SIGCHLD 시그널 처리와 좀비 프로세스 방지
- 환경변수 확장 ($VAR)과 fork() 상속의 관계

---

## Class 16: Win32 API (kernel32.dll 핵심 함수)

**파일:** `wcl/include/win32.h`, `wcl/src/dlls/kernel32/kernel32.c`, `wcl/tests/file_test.c`

### 핵심 개념: Windows API를 Linux 시스콜로 번역하기

Class 13에서 PE 로더(citcrun)를 만들어 Windows .exe를 메모리에 올리고 실행할 수 있게 되었다. 하지만 당시에는 3개의 인라인 스텁(ExitProcess, GetStdHandle, WriteFile)만 있었다. 이제 **kernel32.dll의 핵심 파일 I/O API**를 구현하여 Windows 프로그램이 실제로 파일을 다룰 수 있게 한다.

이것이 Wine이 하는 일의 본질이다: **Windows API 호출 → POSIX 시스콜 번역**.

#### 1. Handle Table — Windows HANDLE vs Linux fd

Windows와 Linux는 I/O 객체를 완전히 다르게 관리한다:

```
Windows:                              Linux:
┌─────────────────────┐              ┌──────────────────┐
│ 프로세스 핸들 테이블  │              │ task_struct       │
│                     │              │   ->files         │
│ HANDLE → Object     │              │     ->fd_array[]  │
│ 0x100  → File obj   │              │ fd 0 → stdin      │
│ 0x101  → Console    │              │ fd 1 → stdout     │
│ 0x102  → Pipe       │              │ fd 3 → file       │
└─────────────────────┘              └──────────────────┘

HANDLE = 불투명 포인터 (void*)        fd = 단순 정수 (int)
CloseHandle(h)                        close(fd)
ReadFile(h, ...)                      read(fd, ...)
```

우리의 구현: 유저스페이스 배열로 커널의 핸들 테이블을 흉내냄.

```c
#define MAX_HANDLES    256
#define HANDLE_OFFSET  0x100   /* NULL/fd와 혼동 방지 */

struct handle_entry {
    enum handle_type type;     /* FREE, FILE, CONSOLE */
    int fd;                    /* Linux file descriptor */
    uint32_t access;           /* GENERIC_READ 등 */
};

static struct handle_entry handle_table[MAX_HANDLES];

/* HANDLE 값 = (인덱스 + 0x100)을 void*로 캐스팅 */
/* HANDLE 0x100 → 인덱스 0 → handle_table[0].fd → Linux fd 0 */
```

#### 2. Path Translation — C:\path → /linux/path

Windows와 Linux의 경로 체계 차이:

| 차이점 | Windows | Linux |
|--------|---------|-------|
| 루트 | 드라이브 문자 (C:, D:) | 단일 루트 (/) |
| 구분자 | 백슬래시 (\) | 슬래시 (/) |
| 대소문자 | 무시 | 구분 |

Wine은 `~/.wine/drive_c/`로 드라이브를 매핑한다. 우리는 단순화:

```c
/* "C:\Users\test.txt" → "/Users/test.txt" */
static void translate_path(const char *win_path, char *linux_path, size_t size)
{
    /* 1. 드라이브 문자 제거 (C: → "") */
    /* 2. 백슬래시 → 슬래시 변환 */
    /* 3. 상대 경로는 그대로 유지 */
}
```

#### 3. Error Code Mapping — GetLastError/errno

```
Linux:    if (open() == -1) → errno 확인
Windows:  if (CreateFile() == INVALID_HANDLE_VALUE) → GetLastError() 호출

errno는 __thread 변수    →    TEB.LastErrorValue도 스레드별
```

TEB (Thread Environment Block): Windows 커널이 각 스레드에 할당하는 구조체. `__thread` 변수로 흉내냄.

```c
static __thread uint32_t last_error = 0;  /* TEB.LastErrorValue */

/* errno → Win32 에러 코드 */
ENOENT  → ERROR_FILE_NOT_FOUND (2)
EACCES  → ERROR_ACCESS_DENIED  (5)
EEXIST  → ERROR_ALREADY_EXISTS (183)
EBADF   → ERROR_INVALID_HANDLE (6)
```

#### 4. CreateFileA — 플래그 매핑

CreateFile은 Windows 파일 I/O의 핵심. POSIX `open()`에 매핑:

**접근 권한:**
| Windows | Linux |
|---------|-------|
| GENERIC_READ | O_RDONLY |
| GENERIC_WRITE | O_WRONLY |
| GENERIC_READ \| WRITE | O_RDWR |

**생성 모드:**
| Windows | Linux | 파일 존재 | 파일 없음 |
|---------|-------|----------|----------|
| CREATE_NEW | O_CREAT \| O_EXCL | 실패 | 생성 |
| CREATE_ALWAYS | O_CREAT \| O_TRUNC | 덮어쓰기 | 생성 |
| OPEN_EXISTING | 0 | 열기 | 실패 |
| OPEN_ALWAYS | O_CREAT | 열기 | 생성 |
| TRUNCATE_EXISTING | O_TRUNC | 비우기 | 실패 |

#### 5. 구현된 API 목록 (11개)

| # | Win32 함수 | POSIX 매핑 | 역할 |
|---|-----------|-----------|------|
| 1 | ExitProcess() | _exit() | 프로세스 종료 |
| 2 | GetStdHandle() | 핸들 테이블 조회 | 표준 핸들 반환 |
| 3 | CreateFileA() | open() | 파일 열기/생성 |
| 4 | WriteFile() | write() | 데이터 쓰기 |
| 5 | ReadFile() | read() | 데이터 읽기 |
| 6 | CloseHandle() | close() | 핸들 닫기 |
| 7 | GetFileSize() | fstat() | 파일 크기 |
| 8 | SetFilePointer() | lseek() | 위치 이동 |
| 9 | DeleteFileA() | unlink() | 파일 삭제 |
| 10 | GetLastError() | (TLS 변수) | 에러 코드 조회 |
| 11 | SetLastError() | (TLS 변수) | 에러 코드 설정 |

### 아키텍처 변경: 인라인 스텁 → kernel32 모듈 분리

Class 13에서는 스텁 함수 3개가 citcrun.c 안에 인라인으로 있었다. Class 16에서 kernel32.c로 분리:

```
Before (Class 13):                    After (Class 16):
┌─────────────────────┐              ┌──────────────┐    ┌──────────────┐
│ citcrun.c           │              │ citcrun.c    │    │ kernel32.c   │
│                     │              │              │    │              │
│  stub_ExitProcess() │              │  find_stub() │───>│  핸들 테이블   │
│  stub_GetStdHandle()│     →        │  kernel32_   │    │  경로 변환     │
│  stub_WriteFile()   │              │   stub_table │    │  에러 매핑     │
│  stub_table[3]      │              │              │    │  API 11개     │
│  find_stub()        │              │              │    │  stub_table[] │
│  pe_resolve_imports │              │              │    │              │
└─────────────────────┘              └──────────────┘    └──────────────┘
                                      PE 로더              Win32 API 구현
```

### 수정/생성된 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `wcl/include/win32.h` | 새로 생성 | Windows 타입 + 상수 + 에러 코드 |
| `wcl/src/dlls/kernel32/kernel32.h` | 새로 생성 | stub_entry 구조체 + 인터페이스 |
| `wcl/src/dlls/kernel32/kernel32.c` | 새로 생성 | 핸들 테이블 + 경로 변환 + API 11개 |
| `wcl/src/loader/citcrun.c` | 수정 | 인라인 스텁 제거, kernel32 연동 |
| `wcl/src/loader/Makefile` | 수정 | kernel32.c 추가 컴파일 |
| `wcl/tests/file_test.c` | 새로 생성 | 파일 I/O 테스트 프로그램 |
| `wcl/tests/Makefile` | 수정 | file_test.exe 빌드 추가 |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | file_test.exe 배포 |

### 사용법

```bash
# 기존 테스트 (하위 호환성 확인)
citcrun /opt/wcl-tests/hello.exe

# 새 파일 I/O 테스트
citcrun /opt/wcl-tests/file_test.exe

# 예상 출력:
=== Win32 File I/O Test ===

[1] CreateFileA("test.txt", WRITE, CREATE_ALWAYS)... OK
[2] WriteFile("Hello from Win32 File I/O!\n")... OK (27 bytes)
[3] CloseHandle... OK
[4] CreateFileA("test.txt", READ, OPEN_EXISTING)... OK
[5] GetFileSize... 27 bytes
[6] ReadFile... OK (27 bytes): Hello from Win32 File I/O!
[7] CloseHandle... OK
[8] DeleteFileA("test.txt")... OK
[9] CreateFileA(OPEN_EXISTING) after delete... FAIL (expected!) error=2

=== All tests passed! ===
```

### 문제 & 해결

이번 Class에서는 빌드/실행 모두 한 번에 성공했다. 문제가 발생하지 않은 이유:

1. **모듈 분리가 깔끔했다** — kernel32.c는 citcrun.c의 기존 스텁을 확장한 것이므로, 호출 규약(ms_abi)이나 IAT 패치 메커니즘에서 새로운 문제가 없었다.
2. **Class 13의 기반이 탄탄했다** — PE 로더의 임포트 해석 흐름(find_stub → stub_table → IAT 기록)을 그대로 재사용. stub_table만 외부(kernel32_stub_table)로 교체하면 됐다.
3. **POSIX API와 1:1 매핑** — CreateFileA→open(), ReadFile→read() 등 직관적인 매핑이 대부분이라 변환 로직에서 실수할 여지가 적었다.

### 주요 학습 포인트

- Windows HANDLE은 커널 Object Table의 인덱스 — Linux fd와 같은 역할이지만 불투명 포인터
- 경로 변환(C:\ → /)은 Wine의 가장 기본적인 호환성 메커니즘
- GetLastError/SetLastError는 __thread(TLS)로 TEB를 흉내낼 수 있다
- CreateFileA의 5가지 생성 모드는 POSIX O_CREAT, O_EXCL, O_TRUNC 조합으로 매핑
- ms_abi 속성으로 Windows↔Linux 호출 규약을 투명하게 변환
- 모듈 분리(citcrun → kernel32): 이후 user32, gdi32 등 DLL 추가의 기반

---

## Class 17: 데스크탑 셸 (태스크바 + 앱 런처)

> **핵심:** Wayland의 layer-shell 개념을 배우고, CDP 프로토콜을 확장하여 패널(panel) surface를 지원. 데스크탑 셸(citcshell)이 태스크바를 표시하고 앱을 실행.

### 배경: 데스크탑 셸이란?

```
컴포지터(서버)              셸(클라이언트)
+---------------------+    +-------------------+
| 윈도우 합성          |    | 태스크바 그리기    |
| 입력 라우팅          |←→ | 앱 런처 버튼      |
| 패널 관리            |CDP | 시계 표시          |
| Z-order 관리         |    | fork+exec 앱 실행  |
+---------------------+    +-------------------+
```

**컴포지터 ≠ 셸.** 이것은 모든 현대 데스크탑의 핵심 아키텍처:
- **Windows:** DWM(컴포지터) + explorer.exe(셸 = 태스크바 + 시작 메뉴)
- **macOS:** Quartz Compositor + Dock + Finder + Menu Bar
- **GNOME:** Mutter(컴포지터) + gnome-shell(패널 + Activities)
- **KDE:** KWin(컴포지터) + plasmashell(패널 + 위젯)

셸은 별도 프로세스로, 프로토콜을 통해 컴포지터와 통신합니다.

### 핵심 개념 1: Layer Shell (레이어 셸)

```
Layer 계층 (아래→위):

  BACKGROUND    배경 이미지 (월페이퍼)
  BOTTOM        일반 윈도우들
  TOP           패널, 독, 알림              ← 태스크바는 여기
  OVERLAY       잠금 화면, 스크린샷 도구
  [커서]        항상 최상위
```

**Wayland에서는 wlr-layer-shell** 프로토콜이 이 역할:
```
zwlr_layer_surface_v1.set_layer(TOP)
zwlr_layer_surface_v1.set_anchor(BOTTOM | LEFT | RIGHT)
zwlr_layer_surface_v1.set_size(0, 32)  /* 전체 너비, 높이 32px */
zwlr_layer_surface_v1.set_exclusive_zone(32)  /* 다른 윈도우 침범 금지 */
```

**CDP에서는 단순화:**
```c
CDP_REQ_SET_PANEL = 7   /* surface를 패널로 전환 */

struct cdp_set_panel {
    uint32_t surface_id;
    uint32_t edge;       /* 0=bottom */
    uint32_t height;     /* 패널 높이 */
};
```

컴포지터가 패널 수신 시:
1. 윈도우를 화면 하단에 고정
2. 전체 너비로 확장
3. 타이틀바/테두리 제거
4. 항상 일반 윈도우 위에 렌더링
5. 드래그 불가

### 핵심 개념 2: 패널 렌더링 분리 (Painter's Algorithm 확장)

```c
/* render_frame() — 레이어 순서 */

/* 1. 배경 */
render_background(buf);

/* 2. 일반 윈도우만 (패널 제외) */
for (i = 0; i < num_windows; i++) {
    if (!windows[i].is_panel)
        render_window(buf, &windows[i]);
}

/* 3. 패널 윈도우 (항상 일반 윈도우 위) */
for (i = 0; i < num_windows; i++) {
    if (windows[i].is_panel)
        render_window(buf, &windows[i]);
}

/* 4. 커서 (최상위) */
render_cursor(buf);
```

패널 윈도우의 render_window()는 타이틀바/테두리를 건너뛰고 직접 blit만 수행.

### 핵심 개념 3: 패널 입력 처리

```
패널 클릭 시:
  ✕ 포커스 변경 (다른 윈도우의 포커스를 빼앗지 않음)
  ✕ 드래그 시작 (타이틀바가 없으므로 불가)
  ○ CDP 포인터 이벤트 전달 (버튼 클릭 감지)
  ○ CDP 포인터 모션 전달 (hover 효과)
```

window_at_point()에서 패널을 먼저 검색:
```c
/* 패널 먼저 (항상 최상위이므로) */
for (i = 0; i < num_windows; i++)
    if (windows[i].is_panel && hit_test(...)) return i;

/* 일반 윈도우 (뒤→앞 Z-order) */
for (i = num_windows - 1; i >= 0; i--)
    if (!windows[i].is_panel && hit_test(...)) return i;
```

### 핵심 개념 4: fork + exec (앱 실행)

```
버튼 클릭 → launch_app("/usr/bin/citcterm")

  fork()
    ├── 부모 (citcshell): 계속 이벤트 루프
    └── 자식:
         setsid()           /* 새 세션 (부모와 분리) */
         execl(path, ...)   /* 자식을 새 프로그램으로 교체 */
```

**Unix의 2단계 프로세스 생성:**
| | Windows | Unix |
|---|---------|------|
| 생성 | CreateProcess() 1회 | fork() → exec() 2단계 |
| 장점 | 간단 | fork~exec 사이에 fd 조작 가능 |

**좀비 프로세스 방지:**
```c
/* SIGCHLD 핸들러 — 자식이 종료될 때 호출 */
void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;  /* 모든 종료된 자식 회수 */
}
```

### 핵심 개념 5: poll() 타임아웃 기반 시계

```c
while (running) {
    int ret = poll(&pfd, 1, 1000);  /* 1초 타임아웃 */

    if (ret > 0)        /* 소켓 이벤트 → 처리 */
        cdp_dispatch(conn);

    /* 타임아웃 또는 이벤트 후 → 시계 업데이트 */
    render_panel();     /* /proc/uptime 읽어서 시계 그리기 */
    cdp_commit(win);    /* 화면 갱신 */
}
```

poll() 타임아웃 = 이벤트 루프에 주기적 작업을 끼워넣는 표준 패턴.

### 구현 결과

```
compositor &
citcshell &

화면:
+--------------------------------------------------------+
|  [일반 윈도우들...]                                      |
|                                                        |
|                                                        |
+--------------------------------------------------------+
| [CITC OS] | [Terminal] [Demo]          00:05:23        |  ← 패널
+--------------------------------------------------------+
```

### 수정/생성한 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `display/protocol/cdp_proto.h` | 수정 | CDP_REQ_SET_PANEL(=7) + struct cdp_set_panel |
| `display/protocol/cdp_client.h` | 수정 | cdp_set_panel() 편의 함수 |
| `display/compositor/src/compositor.c` | 수정 | is_panel 플래그, 패널 렌더링/입력 처리 |
| `display/shell/src/citcshell.c` | **새로 생성** | 데스크탑 셸 (~500줄) |
| `display/shell/Makefile` | **새로 생성** | 빌드 파일 |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | 3.88단계 추가 |

### 사용법

```bash
# QEMU에서:
compositor &
citcshell &

# [Terminal] 버튼 클릭 → citcterm 실행
# [Demo] 버튼 클릭 → cdp_demo 실행
```

### 배운 점

- Wayland의 layer-shell은 패널/독/알림 같은 특수 UI를 지원하는 프로토콜 확장
- 데스크탑 셸은 컴포지터의 일부가 아닌 별도 클라이언트 프로세스
- 패널은 "포커스를 빼앗지 않는 최상위 윈도우"라는 특수한 동작이 필요
- fork+exec = Unix 프로세스 생성의 핵심 패턴 (Windows의 CreateProcess와 대비)
- poll() 타임아웃으로 이벤트 루프에 주기적 작업을 자연스럽게 통합

### 문제 & 해결 (총 3개)

**문제 1: Header-only 라이브러리의 `-Werror=unused-function`**

```
error: 'cdp_set_panel' defined but not used [-Werror=unused-function]
error: 'cdp_destroy_surface' defined but not used [-Werror=unused-function]
error: 'cdp_request_frame' defined but not used [-Werror=unused-function]
```

`cdp_client.h`는 header-only 라이브러리로, `static` 함수들이 정의되어 있다.
이 헤더를 include하는 모든 C 파일이 모든 함수를 사용하는 건 아니므로,
`-Werror`와 함께 컴파일하면 "정의만 하고 사용하지 않음" 에러가 발생.

**해결:** `__attribute__((unused))`를 static 함수에 추가:
```c
__attribute__((unused))
static void cdp_set_panel(struct cdp_conn *conn, ...) { ... }
```
이 속성은 "이 함수를 사용하지 않아도 경고하지 마라"는 의미.
GCC/Clang 공통으로 지원됨.

**교훈:** Header-only 라이브러리에서 static 함수를 제공할 때는
`__attribute__((unused))`를 기본으로 붙이자.

---

**문제 2: POLLHUP 미처리 — 클라이언트 종료 후 슬롯 미회수**

클라이언트를 여러 번 실행/종료하면 "클라이언트 슬롯 없음 (최대 4)" 에러 발생.

```c
/* 기존 코드 — POLLIN만 체크 */
if (!(fds[i].revents & POLLIN))
    continue;
```

클라이언트가 종료(소켓 close)하면 `poll()`은 `POLLHUP`을 반환한다.
하지만 기존 코드는 `POLLIN`만 체크했으므로, `POLLHUP`만 온 경우
disconnect 핸들러가 호출되지 않아 슬롯이 영원히 점유됨.

**해결:**
```c
/* POLLHUP/POLLERR도 체크 → disconnect 감지 */
if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR)))
    continue;
```

`POLLHUP`이나 `POLLERR` 시에도 `cdp_handle_client_msg()` 호출 →
`read()`가 0 반환 → disconnect cleanup → 슬롯 회수.

**교훈:** `poll()` 기반 서버에서는 항상 `POLLIN | POLLHUP | POLLERR`을
함께 체크해야 클라이언트 종료를 놓치지 않는다.

---

**문제 3: X 버튼으로 닫아도 리소스 미해제 — 3겹 슬롯 누수**

X 버튼으로 윈도우를 닫으면 화면에서만 사라질 뿐,
클라이언트 소켓/surface/윈도우 슬롯이 모두 살아있었다:

```c
/* 기존 X 버튼 핸들러 — visible만 끔 */
w->visible = 0;
```

| 리소스 | 해제됨? | 결과 |
|--------|---------|------|
| 클라이언트 슬롯 (fd) | ✕ | 최대 4개 소진 |
| surface 슬롯 | ✕ | 공유메모리 누수 |
| 윈도우 슬롯 | ✕ | 최대 8개 소진 |

3가지 리소스가 동시에 누수되어 4번만 켰다 끄면 더 이상 아무것도 실행 불가.

**해결 (3단계):**

1. **`cdp_disconnect_client()` 함수 분리** — 클라이언트 전체 정리 로직을
   별도 함수로 추출. disconnect와 X 버튼 양쪽에서 호출:
```c
static void cdp_disconnect_client(int client_idx)
{
    /* surface 정리: 윈도우 숨기기 + cdp_surface_idx 리셋 + munmap + close(shm_fd) */
    /* 소켓 닫기: close(client_fd) */
    /* 슬롯 해제: clients[idx].fd = -1 */
}
```

2. **X 버튼 핸들러에서 `cdp_disconnect_client()` 호출:**
```c
/* X 버튼 클릭 → 윈도우의 surface → client 찾아서 완전 정리 */
int sidx = w->cdp_surface_idx;
if (sidx >= 0 && cdp.surfaces[sidx].active)
    cdp_disconnect_client(cdp.surfaces[sidx].client_idx);
```

3. **`window_create()`에서 빈 슬롯 재사용:**
```c
/* 닫힌 윈도우(visible=0, cdp_surface_idx=-1) 슬롯을 먼저 재사용 */
for (int i = 0; i < num_windows; i++) {
    if (!windows[i].visible && windows[i].cdp_surface_idx < 0) {
        idx = i;
        break;
    }
}
/* 없으면 num_windows++ */
```

**교훈:** 리소스를 여러 계층에 걸쳐 할당하면 (클라이언트→surface→윈도우),
해제 시에도 모든 계층을 빠짐없이 정리해야 한다.
"화면에서 사라짐 ≠ 리소스 해제"에 주의.

---

## 공통 교훈

### DRM ioctl 2-pass 황금 규칙

**이 프로젝트에서 3번 반복된 버그:**

> `count_X > 0`인 **모든** 배열에 대해 유효한 포인터를 전달해야 한다.
> 하나라도 NULL이면 커널이 EFAULT(Bad address)를 반환한다.

| ioctl | 필수 배열 |
|-------|----------|
| GETRESOURCES | connector_id_ptr, crtc_id_ptr, **encoder_id_ptr**, **fb_id_ptr** |
| GETCONNECTOR | modes_ptr, encoders_ptr, **props_ptr**, **prop_values_ptr** |

빠지기 쉬운 것: 직접 사용하지 않는 배열도 반드시 할당해야 함!

### 정적 링킹의 중요성

initramfs에는 공유 라이브러리가 없으므로 모든 바이너리를 `-static`으로 빌드해야 한다.
안 하면 "not found" 에러가 나는데, 이는 바이너리 자체가 없는 게 아니라 동적 링커(`ld-linux.so`)가 없기 때문이다.

### NTFS vs ext4

WSL에서 OS를 개발할 때, initramfs 생성은 반드시 ext4 파일시스템에서 해야 한다.
NTFS는 다음을 지원하지 않는다:
- `mknod` (장치 파일 생성)
- 심볼릭 링크 (제한적)
- Unix 퍼미션 (chmod)

해결: `/tmp`(WSL의 ext4)에서 작업.

### C에서 unsigned 리터럴 주의

```c
// LINUX_REBOOT_CMD_HALT = 0xCDEF0123 (INT_MAX 초과!)
// volatile int와 비교하면 sign-compare 경고
volatile unsigned int shutdown_cmd;  // unsigned 사용!
```

### 디버그 출력의 가치

DRM 연결 문제처럼 "왜 실패하는지" 모를 때, 중간 값을 출력하는 것이 가장 효과적이다:
```c
printf("커넥터 %u: connection=%u, modes=%u\n",
       conn.connector_id, conn.connection, conn.count_modes);
```
이 한 줄로 "커넥터는 연결됨(1), 모드도 있음(1)" 확인 → 2nd pass가 실패하는 것으로 범위 좁힘.

### 에러 메시지를 숨기지 말자

```bash
# 나쁜 예 — 왜 실패하는지 알 수 없음
insmod module.ko 2>/dev/null && echo "OK" || echo "실패"

# 좋은 예 — 에러 원인이 바로 보임
ERR=$(insmod module.ko 2>&1) && echo "OK" || echo "실패: ${ERR}"
```

Class 11에서 커널 모듈이 전부 실패했는데, `2>/dev/null`로 에러가 숨겨져 있어 "이미 로딩됨"으로 오인했다. 실제 원인은 커널 버전 불일치(WSL 6.6 vs Ubuntu 6.8). 에러 메시지를 표시했으면 즉시 알 수 있었을 것.

### 입력 장치 판별 순서의 중요성

하나의 입력 장치가 여러 이벤트 타입을 동시에 지원할 수 있다:

```
QEMU Virtio Tablet:
  EV_KEY (버튼) + EV_REL (스크롤 휠) + EV_ABS (마우스 이동)
```

`EV_REL`을 먼저 체크하면 "상대 마우스"로 잘못 분류되어 `EV_ABS` 이벤트가 무시된다.
규칙: **가장 구체적인 조건을 먼저 체크** (`EV_ABS + ABS_X` → `EV_REL` → `EV_KEY`).

### WSL에서 QEMU 개발 시 커널 버전 주의

```
WSL 커널:   6.6.87.2-microsoft-standard-WSL2 (uname -r 결과)
Ubuntu 커널: 6.8.0-100-generic (build/vmlinuz, QEMU에서 부팅)
```

이 두 커널은 다르다! 모듈, 설정, 기능이 모두 다름. `uname -r`에 의존하지 말고, 실제 부팅할 커널 버전을 명시적으로 찾아야 한다.

---

## 프로젝트 파일 구조 요약

```
citc_os/
├── system/
│   ├── citcinit/              # Init 시스템 (PID 1)
│   │   ├── src/
│   │   │   ├── main.c         # PID 1 메인 (마운트, 콘솔, 시그널, 메인루프)
│   │   │   ├── service.h/c    # 서비스 관리자 (등록, 시작, 의존성, 재시작)
│   │   │   ├── config.h/c     # .conf 파일 파서
│   │   │   └── shutdown.c     # shutdown/reboot/halt 명령어
│   │   ├── services/          # 서비스 설정 파일
│   │   │   ├── syslog.conf
│   │   │   ├── klogd.conf
│   │   │   └── network.conf
│   │   └── scripts/
│   │       └── network-setup.sh
│   ├── citcsh/                # 커스텀 쉘 (파이프, 리다이렉션)
│   │   └── src/
│   │       └── citcsh.c       # REPL + 토크나이저 + 파서 + 실행 엔진
│   └── citcpkg/               # 패키지 매니저
│       └── src/
│           ├── main.c         # CLI 엔트리포인트
│           ├── package.h/c    # 로컬 패키지 설치/제거
│           └── repo.h/c       # 원격 저장소 + DFS 의존성 해결
│
├── display/
│   ├── fbdraw/src/            # 프레임버퍼 그래픽 데모
│   │   ├── fbdraw.c           # /dev/fb0 직접 그리기
│   │   └── font8x8.h          # 8x8 비트맵 폰트
│   ├── drmdraw/src/           # DRM/KMS 그래픽 데모
│   │   └── drmdraw.c          # /dev/dri/card0 + 더블 버퍼링
│   ├── compositor/src/        # 윈도우 관리자 (evdev + DRM + CDP 서버)
│   │   └── compositor.c       # 입력, 윈도우 관리, 컴포지팅, CDP 서버
│   ├── protocol/              # CITC Display Protocol (Wayland 개념)
│   │   ├── cdp_proto.h        # 프로토콜 정의 (서버/클라이언트 공유)
│   │   ├── cdp_client.h       # 클라이언트 라이브러리 (header-only)
│   │   ├── cdp_demo.c         # 데모 클라이언트 앱
│   │   └── Makefile
│   ├── terminal/              # 터미널 에뮬레이터 (PTY + CDP)
│   │   ├── src/
│   │   │   └── citcterm.c     # 그래픽 터미널 (ANSI 파서 + poll 이벤트 루프)
│   │   └── Makefile
│   └── shell/                 # 데스크탑 셸 (태스크바 + 앱 런처)
│       ├── src/
│       │   └── citcshell.c    # CDP 패널 클라이언트 (버튼, 시계, fork+exec)
│       └── Makefile
│
├── wcl/                       # Windows Compatibility Layer
│   ├── include/
│   │   ├── pe.h               # PE 포맷 구조체 정의
│   │   └── win32.h            # Windows 타입 + 상수 + 에러 코드
│   ├── src/
│   │   ├── loader/
│   │   │   ├── citcrun.c      # PE 로더 + CLI
│   │   │   └── Makefile
│   │   └── dlls/
│   │       └── kernel32/
│   │           ├── kernel32.c # 핸들 테이블 + 경로 변환 + API 11개
│   │           └── kernel32.h # stub_entry 구조체 + 인터페이스
│   └── tests/
│       ├── hello_win.c        # 테스트: 콘솔 출력
│       ├── file_test.c        # 테스트: 파일 I/O
│       └── Makefile           # MinGW 크로스 컴파일
│
├── tools/
│   ├── mkrootfs/build-initramfs.sh  # initramfs 생성
│   ├── mkiso/build-iso.sh           # ISO 이미지 생성
│   ├── run-qemu.sh                  # QEMU 실행 (3가지 모드)
│   ├── build-sample-packages.sh     # 샘플 패키지 빌드
│   └── serve-repo.sh               # 패키지 서버 (Docker)
│
├── build.sh                   # 마스터 빌드 스크립트
└── learn.md                   # 이 파일!
```

---

## 다음 단계

**Class 16 완료 후 예상 로드맵:**

1. ~~**PE 로더** — Windows .exe 파일 로딩 (WCL Phase 2 시작)~~ ✅ Class 13
2. ~~**터미널 에뮬레이터** — CDP 클라이언트로 진짜 터미널 구현~~ ✅ Class 14
3. ~~**쉘** — 커스텀 쉘 (명령어 파싱, 파이프, 리다이렉션)~~ ✅ Class 15
4. ~~**Win32 API** — CreateFile, ReadFile 등 핵심 함수 구현~~ ✅ Class 16
5. ~~**데스크탑 쉘** — 태스크바, 앱 런처, 윈도우 관리 UI~~ ✅ Class 17
6. **Win32 프로세스/스레드** — CreateProcess, CreateThread 구현
7. **Win32 GUI 기초** — user32.dll (CreateWindowEx, MessageBox 등)

---

## Class 18: 부팅 환경 개선 (듀얼 콘솔 + 시리얼 쉘)

> **핵심:** Linux 콘솔 아키텍처를 이해하고, pipe+fork tee 패턴으로 듀얼 출력 구현. VGA 프레임버퍼의 폰트 제한을 파악하고 해결. 시리얼 포트에 별도 쉘을 띄워 원격 명령어 실행 가능하게 함.

### 배경: 3가지 문제

QEMU에서 CITC OS를 부팅했을 때 3가지 문제가 있었다:

1. **화면(QEMU 창)에 한글이 네모(□)로 표시** — 로그를 읽을 수 없음
2. **시리얼 콘솔(WSL 터미널)에 citcinit 로그가 안 나옴** — 화면에만 출력됨
3. **WSL 터미널에서 명령어 입력 불가** — 출력만 보이고 입력은 QEMU 화면에서만 가능

### 핵심 개념 1: Linux 콘솔 아키텍처

```
커널 부팅 파라미터:
  console=ttyS0,115200 console=tty0

규칙: 마지막 console= 파라미터가 /dev/console이 된다!

  console=ttyS0 console=tty0
                         ↑ /dev/console = tty0 (화면)

  console=tty0 console=ttyS0
                         ↑ /dev/console = ttyS0 (시리얼)
```

**tty0 vs ttyS0 vs /dev/console:**

| 장치 | 설명 | 물리적 대상 |
|------|------|------------|
| `/dev/tty0` | VGA 물리 콘솔 | QEMU 화면 |
| `/dev/ttyS0` | 첫 번째 시리얼 포트 | WSL 터미널 (`-serial stdio`) |
| `/dev/console` | 커널이 지정한 주 콘솔 | 마지막 `console=` 파라미터의 장치 |

citcinit은 `/dev/console`을 열어서 stdin/stdout/stderr에 연결하므로, `/dev/console`이 어느 장치인지가 로그가 어디에 보이는지를 결정한다.

### 핵심 개념 2: VGA 프레임버퍼 폰트 제한

```
커널의 VGA 콘솔 폰트 = 256자 비트맵 (lat0-16 등)

지원: ASCII (A-Z, 0-9, 특수문자)
      서유럽 문자 (ü, ñ, é 등)
      일부 Box-Drawing 문자 (─, │, ┌ 등) → 폰트에 따라 다름

미지원: 한글 (가-힣)     → □□□로 표시
        한자 (漢字)      → □□□로 표시
        특수 Unicode (→) → □로 표시
```

**해결:** 런타임에 출력되는 모든 문자열을 ASCII/영어로 변경:

| 변경 대상 | 변경 전 | 변경 후 |
|-----------|---------|---------|
| citcinit 로그 | `"시스템 초기화 완료"` | `"System initialization complete"` |
| 마운트 화살표 | `"proc → /proc"` | `"proc -> /proc"` |
| 테이블 선 | `"───────"` | `"-------"` |
| 모듈 로딩 스크립트 | `"모듈 로딩 완료"` | `"Module loading complete"` |
| GRUB 메시지 | `"CITC OS를 부팅합니다..."` | `"Booting CITC OS..."` |

**주의:** 소스 코드의 **주석**은 한글 그대로 유지. 주석은 화면에 출력되지 않으므로 문제 없다.

### 핵심 개념 3: pipe + fork tee 패턴 (듀얼 출력)

모든 printf() 출력을 화면과 시리얼 양쪽에 동시에 보내는 방법:

```
기존:
  citcinit → stdout (fd 1) → /dev/console (tty0) → 화면만

듀얼 출력:
  citcinit → stdout (fd 1) → pipe write end
                                    ↓
                              [tee child process]
                                 ↓           ↓
                          /dev/console    /dev/ttyS0
                          (화면)         (시리얼/WSL)
```

**구현:**
```c
static void setup_dual_output(void)
{
    int serial_fd = open("/dev/ttyS0", O_WRONLY | O_NOCTTY);
    int console_wr = open("/dev/console", O_WRONLY);
    int pipefd[2];
    pipe(pipefd);

    fflush(stdout);  /* 기존 버퍼 flush */

    if (fork() == 0) {
        /* 자식: tee 프로세스 */
        char buf[512];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            write(console_wr, buf, n);  /* 화면 */
            write(serial_fd, buf, n);   /* 시리얼 */
        }
        _exit(0);
    }

    /* 부모: stdout/stderr를 pipe로 전환 */
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);

    setvbuf(stdout, NULL, _IOLBF, 0);  /* 줄 단위 버퍼링 */
}
```

**중요: 호출 타이밍!**
```
setup_console()              ← /dev/console만 (기본)
print_banner()
mount_early_filesystems()    ← devtmpfs 마운트 → /dev/ttyS0 생성됨
create_dev_nodes()
setup_dual_output()          ← 여기서 호출! /dev/ttyS0가 존재해야 함
```

`/dev/ttyS0`는 devtmpfs가 마운트된 후에야 존재한다. 마운트 전에 `open("/dev/ttyS0")`하면 실패한다.

**pipe 통과 시 버퍼링 문제:**
```
터미널에 직접 출력 → 줄 단위 버퍼링 (line-buffered)
pipe에 출력        → 전체 버퍼링 (fully-buffered, 4KB)

→ setvbuf(stdout, NULL, _IOLBF, 0) 으로 강제 줄 단위 설정
  안 하면 로그가 4KB 모일 때까지 안 보임!
```

### 핵심 개념 4: 시리얼 쉘 (원격 명령어 입력)

듀얼 출력만으로는 WSL 터미널에서 **보기만** 가능하고 **입력은 불가**:

```
현재 stdin 흐름:
  QEMU 화면 키보드 → /dev/console (tty0) → shell의 stdin
  WSL 터미널 타이핑 → /dev/ttyS0 → (아무도 안 읽음!)
```

**해결:** `/dev/ttyS0`에 별도 쉘을 띄움:

```c
static pid_t spawn_serial_shell(void)
{
    int serial_fd = open("/dev/ttyS0", O_RDWR | O_NOCTTY);
    if (fork() == 0) {
        setsid();
        dup2(serial_fd, STDIN_FILENO);   /* 시리얼에서 입력 */
        dup2(serial_fd, STDOUT_FILENO);  /* 시리얼로 출력 */
        dup2(serial_fd, STDERR_FILENO);
        execve("/bin/sh", ...);
    }
    return pid;
}
```

결과: 2개의 독립 쉘이 동시에 실행:

```
[QEMU 화면]    ←→ /dev/console ←→ shell #1 (메인)
[WSL 터미널]   ←→ /dev/ttyS0   ←→ shell #2 (시리얼)
```

### 수정/생성한 파일

| 파일 | 작업 | 설명 |
|------|------|------|
| `system/citcinit/src/main.c` | 수정 | setup_console() 분리 + setup_dual_output() + spawn_serial_shell() + 한글→영어 |
| `system/citcinit/src/service.c` | 수정 | 한글 로그→영어, Unicode 테이블 문자→ASCII |
| `system/citcinit/src/config.c` | 수정 | 한글 로그→영어 |
| `tools/mkrootfs/build-initramfs.sh` | 수정 | load-input-modules.sh 한글→영어 |
| `tools/mkiso/build-iso.sh` | 수정 | GRUB echo 메시지 한글→영어 |

### 문제 & 해결

**문제 1: /dev/ttyS0가 없어서 듀얼 출력 실패**

처음에는 `setup_console()` 안에서 한꺼번에 듀얼 출력을 설정했는데, 이 함수는 `mount_early_filesystems()` 이전에 호출됨. devtmpfs가 아직 마운트되지 않아 `/dev/ttyS0`가 존재하지 않음.

```
호출 순서:
  setup_console()           ← 여기서 /dev/ttyS0 open → FAIL!
  mount_early_filesystems() ← devtmpfs 마운트 (이제야 /dev/ttyS0 생김)
```

**해결:** 함수를 2개로 분리:
- `setup_console()` — 부팅 초기, `/dev/console`만
- `setup_dual_output()` — devtmpfs 마운트 후 호출

**교훈:** 장치 파일은 devtmpfs 마운트 후에만 사용 가능. 부팅 순서를 항상 의식해야 한다.

---

**문제 2: 화면에 한글이 네모(□)로 표시**

원인이 다층적이었다:

| 계층 | 파일 | 한글 출처 |
|------|------|----------|
| citcinit | main.c, service.c, config.c | 로그 메시지 (printf) |
| 부트 스크립트 | load-input-modules.sh | echo 메시지 |
| GRUB | build-iso.sh 내 grub.cfg | echo 메시지 |

VGA 프레임버퍼 콘솔은 256자 비트맵 폰트만 지원하므로, 한글(수천 자)은 원천적으로 표시 불가.

**해결:** 런타임 출력 문자열을 전부 영어/ASCII로 변경. 소스 코드 주석은 한글 유지 (화면에 안 나오므로).

**교훈:** 프레임버퍼 콘솔에서는 ASCII만 안전. CJK 문자 지원이 필요하면 유저스페이스 폰트 렌더러(fbterm 등)가 필요하다.

### 배운 점

- Linux의 `console=` 파라미터 순서가 `/dev/console`의 정체를 결정
- pipe + fork = stdout을 여러 목적지로 분배하는 Unix 표준 패턴 (tee 명령어의 원리)
- `setvbuf(stdout, _IOLBF)` — pipe 통과 시 반드시 줄 단위 버퍼링 설정
- 장치 파일 사용 시 devtmpfs 마운트 타이밍 확인 필수
- VGA 콘솔의 비트맵 폰트는 256자 한정 → 한글/CJK 불가
- 시리얼 콘솔에 별도 쉘을 띄우면 원격 디버깅/개발이 가능

---

## Class 19: 소켓 활성화 (Socket Activation)

> **핵심:** systemd의 소켓 활성화를 citcinit에 구현. init이 미리 소켓을 생성하고, 클라이언트가 연결하면 서비스를 온디맨드로 시작. LISTEN_FDS 프로토콜로 fd를 전달. 메인 루프를 pause()에서 poll()로 전환하고, self-pipe 트릭으로 시그널과 소켓 이벤트를 통합.

### 배경: 소켓 활성화란?

전통적 서비스 관리:
```
init 시작 → 모든 서비스 순차 시작 → 준비 완료
             ↑ 안 쓰는 서비스도 전부 시작 (메모리 낭비)
             ↑ 서비스 A가 서비스 B의 소켓을 기다림 (순서 의존성)
```

소켓 활성화:
```
init 시작 → 소켓만 미리 생성 → 준비 완료 (빠름!)
             클라이언트 연결 → 그때서야 서비스 시작
             서비스가 fd 3으로 이미 열린 소켓을 받음
```

**장점 3가지:**

| 장점 | 설명 |
|------|------|
| 부팅 속도 | 서비스를 미리 안 띄우므로 빠름 |
| 순서 의존성 해결 | 소켓은 먼저 존재하므로 클라이언트가 바로 연결 가능 |
| 자원 절약 | 실제 사용될 때만 서비스 프로세스 생성 |

**실제 사례:**

- **systemd:** `systemd-socket-activate`, `.socket` 유닛 파일
- **inetd:** 1980년대 Unix의 원조 소켓 활성화
- **launchd (macOS):** Apple의 소켓 활성화 데몬

### 핵심 개념 1: LISTEN_FDS 프로토콜

systemd가 정의한 표준 프로토콜. init이 서비스에게 소켓 fd를 전달하는 방식:

```
init (PID 1):
  1. socket() + bind() + listen()  → listen_fd 생성
  2. fork() → 자식 프로세스
  3. dup2(listen_fd, 3)            → fd 3에 소켓 복제
  4. setenv("LISTEN_FDS", "1")     → "fd가 1개 있다"
  5. setenv("LISTEN_PID", "자식PID") → 보안: 본인 확인용
  6. execve("/usr/sbin/서비스")

서비스:
  1. getenv("LISTEN_FDS") → "1" → fd 3이 소켓
  2. getenv("LISTEN_PID") → 내 PID와 비교 (보안)
  3. accept(3, ...) → 클라이언트 연결 수락
     (socket + bind + listen 건너뜀!)
```

**왜 fd 3?**
```
fd 0 = stdin
fd 1 = stdout
fd 2 = stderr
fd 3 = 첫 번째 전달 소켓  ← LISTEN_FDS의 시작점
fd 4 = 두 번째 전달 소켓  (LISTEN_FDS=2일 때)
...
```

### 핵심 개념 2: self-pipe 트릭

**문제:** 기존 citcinit의 메인 루프는 `pause()`를 사용:

```c
/* 기존 (Class 4~18) */
for (;;) {
    pause();  /* 시그널이 올 때까지 대기 */
    if (got_sigchld) reap_zombies();
    if (shutdown_requested) do_shutdown();
}
```

`pause()`는 시그널만 감지. 소켓의 POLLIN 이벤트는 감지 불가!

**시도:** `poll()`로 전환하면?

```c
/* poll()은 fd 이벤트를 감지할 수 있다 */
poll(fds, nfds, -1);  /* 소켓에 연결이 오면 깨어남! */
```

하지만 `poll()`은 시그널에 깨어나지 않음 (SIGCHLD로 좀비를 수거해야 하는데!).

**해결 — self-pipe 트릭 (Daniel J. Bernstein, 1990년대):**

```
pipe(signal_pipe)   → signal_pipe[0] = 읽기, signal_pipe[1] = 쓰기

시그널 핸들러:
  write(signal_pipe[1], "x", 1)   ← 1바이트 쓰기 (async-signal-safe!)

메인 루프:
  poll({소켓들..., signal_pipe[0]}, ...)
       ↑ 소켓 이벤트              ↑ 시그널 이벤트 (pipe에서 읽기 가능)

→ 소켓이든 시그널이든 poll()이 깨어남!
```

**왜 pipe인가?**

- 시그널 핸들러에서 호출할 수 있는 함수가 매우 제한적 (async-signal-safe)
- `write()`는 async-signal-safe (POSIX 보장)
- `printf()`, `malloc()`, `mutex_lock()` 등은 불가!

```c
/* citcinit의 시그널 핸들러 - write()만 사용 */
static void handle_sigchld(int sig)
{
    (void)sig;
    got_sigchld = 1;

    int saved_errno = errno;       /* errno 보존 (중요!) */
    write(signal_pipe[1], "C", 1); /* pipe에 1바이트 → poll() 깨움 */
    errno = saved_errno;           /* errno 복원 */
}
```

### 핵심 개념 3: poll() 기반 이벤트 루프

```c
for (;;) {
    /* 1. 플래그 확인 (시그널 핸들러가 설정) */
    if (shutdown_requested) { sa_cleanup(); do_shutdown(); }
    if (got_sigchld) { got_sigchld = 0; reap_zombies(); }

    /* 2. 감시할 fd 목록 구성 */
    struct pollfd fds[MAX];
    nfds = sa_build_poll_fds(fds, MAX);
    /*
     * fds[0] = signal_pipe[0]  (시그널 알림)
     * fds[1] = /run/citc-ipc   (IPC 소켓, STOPPED 상태일 때만)
     * fds[2] = /tmp/citc-display-0  (컴포지터 소켓, STOPPED일 때만)
     * ...
     */

    /* 3. 대기 (소켓 연결 or 시그널) */
    if (poll(fds, nfds, -1) > 0)
        sa_handle_events(fds, nfds);
        /*
         * signal_pipe에 POLLIN → pipe 비우기
         * 소켓에 POLLIN → 해당 서비스 시작!
         */
}
```

**STOPPED 상태인 서비스만 감시하는 이유:**
- 이미 실행 중인 서비스의 소켓은 서비스 자체가 accept()
- init은 아직 안 시작된 서비스의 소켓만 감시
- 서비스가 시작되면 poll 목록에서 제거

### 수정한 파일

| 파일 | 작업 |
|------|------|
| `system/citcinit/src/service.h` | socket_path, listen_fd, socket_activated 필드 추가 |
| `system/citcinit/src/service.c` | svc_set_socket(), svc_find_by_listen_fd(), LISTEN_FDS 전달 |
| `system/citcinit/src/socket_activation.h` | 소켓 활성화 API 선언 |
| `system/citcinit/src/socket_activation.c` | 소켓 생성/감시/fd전달 구현 (~280줄) |
| `system/citcinit/src/config.c` | `socket=` 키 파싱 |
| `system/citcinit/src/main.c` | pause() → poll() + self-pipe 통합 |
| `system/citcinit/Makefile` | socket_activation.c 추가 |
| `display/compositor/src/compositor.c` | LISTEN_FDS 감지 → fd 3 사용 |

### 서비스 설정 예시

```ini
# citc-ipc.conf - 소켓 활성화 서비스
name=citc-ipc
exec=/usr/sbin/citc-ipc
type=simple
restart=yes
socket=/run/citc-ipc     ← 이 줄이 소켓 활성화를 트리거
```

`socket=` 키가 있으면:

1. init이 `/run/citc-ipc` 소켓을 미리 생성
2. 서비스를 즉시 시작하지 않음 (STOPPED 상태 유지)
3. 클라이언트가 연결하면 그때 서비스 시작 + fd 전달

### 배운 점

- 소켓 활성화는 부팅 속도와 서비스 순서 의존성을 동시에 해결하는 핵심 기법
- LISTEN_FDS 프로토콜: fd 3부터 시작, LISTEN_PID로 보안 확인
- self-pipe 트릭: 시그널과 I/O 이벤트를 하나의 poll() 루프로 통합
- async-signal-safe 함수만 시그널 핸들러에서 사용 가능 (write OK, printf NO)
- errno 보존: 시그널 핸들러가 errno를 덮어쓸 수 있으므로 save/restore 필수
- pause() → poll() 전환은 단순 시그널 대기에서 범용 이벤트 루프로의 진화

---

## Class 20: IPC 메시지 버스 (간소화 D-Bus)

> **핵심:** D-Bus의 핵심 개념(이름 등록, 메시지 라우팅, 브로드캐스트)을 교육적으로 단순화한 IPC 버스 구현. Unix domain socket + poll() 이벤트 루프로 프로세스 간 통신 데몬을 만듦.

### 배경: D-Bus란?

Linux 데스크탑의 표준 IPC(프로세스 간 통신) 시스템:

```
+-------+    +----------+    +--------+
| 음악  | →  | D-Bus    | →  | 셸     |
| 재생기|    | 메시지   |    | (패널) |
+-------+    | 버스     |    +--------+
             |          |
+-------+    |          |    +--------+
| 패키지| →  |          | →  | 알림   |
| 매니저|    +----------+    | 데몬   |
+-------+                   +--------+
```

**D-Bus가 하는 일:**

1. **이름 등록:** 서비스가 자신의 이름을 등록 (예: `org.freedesktop.NetworkManager`)
2. **메시지 라우팅:** "이 이름의 서비스에게 메시지 전달해줘"
3. **브로드캐스트(시그널):** "모든 구독자에게 알림" (예: "네트워크 연결 변경됨")

**D-Bus vs CITC IPC:**

| 항목 | D-Bus | CITC IPC |
| ------ | ------- | ---------- |
| 코드 규모 | 수만 줄 | ~350줄 |
| 타입 시스템 | 복잡 (시그니처 문자열) | 고정 구조체 |
| 인증 | SASL 기반 | 없음 (단순화) |
| 서비스 이름 | `org.freedesktop.Foo` | 짧은 문자열 (`"display"`) |
| 인트로스펙션 | XML 기반 | 없음 |
| 핵심 개념 | 같음 | 같음 |

### 핵심 개념 1: 프로토콜 설계

모든 메시지 = **헤더(12바이트)** + **페이로드(가변)**

```
+----------+----------+----------+-------------------+
| type(4B) | length(4B)| serial(4B)|   payload(가변)  |
+----------+----------+----------+-------------------+
  ↑ 메시지 종류  ↑ 페이로드 길이  ↑ 요청-응답 매칭
```

**메시지 타입:**

```
클라이언트 → 버스:
  REGISTER(1)    "나는 'display'라는 이름으로 서비스를 제공한다"
  SEND(2)        "'pkgmgr'에게 이 메시지를 전달해줘"
  BROADCAST(3)   "모든 클라이언트에게 알림을 보내라"

버스 → 클라이언트:
  WELCOME(100)   "연결 성공, 너의 ID는 3이다"
  DELIVER(101)   "누군가 너에게 메시지를 보냈다"
  SIGNAL(102)    "누군가 브로드캐스트를 보냈다"
  ERROR(103)     "요청 실패 (이름 중복 등)"
```

**serial 필드의 역할:**
```
클라이언트 A → SEND(serial=42, dest="display", method="get_res")
버스 → 클라이언트 B (display): DELIVER(serial=42, ...)
클라이언트 B → SEND(serial=42, dest="A", ...)  ← 응답
클라이언트 A: "serial 42의 응답이 왔다!"
```

### 핵심 개념 2: IPC 데몬 구조

```c
/* 전역 상태 */
static int listen_fd;                    /* 리스닝 소켓 */
static struct ipc_client clients[32];    /* 클라이언트 배열 */
static volatile int running = 1;         /* 실행 플래그 */

/* 메인 루프 (poll 기반) */
while (running) {
    /* pollfd 배열 구성: listen_fd + 모든 클라이언트 fd */
    struct pollfd fds[33];
    fds[0] = { .fd = listen_fd, .events = POLLIN };
    for (i = 0; i < 32; i++)
        if (clients[i].fd >= 0)
            fds[n++] = { .fd = clients[i].fd, .events = POLLIN };

    poll(fds, n, -1);

    if (fds[0].revents & POLLIN)
        accept_client();          /* 새 클라이언트 연결 */

    for (each client fd with POLLIN)
        handle_client_message();  /* 메시지 처리 */
}
```

**메시지 라우팅 흐름:**

```
클라이언트 A (name="shell")                   클라이언트 B (name="pkgmgr")
     |                                              |
     |-- SEND(dest="pkgmgr", method="install") -->  |
     |                                              |
     |              IPC 데몬:                       |
     |   1. dest="pkgmgr" → find_by_name()          |
     |   2. 클라이언트 B를 찾음                      |
     |   3. DELIVER(sender="shell") → B에 전달       |
     |                                              |
     |  <-- DELIVER(sender="shell", method="install")|
```

### 핵심 개념 3: 헤더 전용 클라이언트 라이브러리

서비스들이 IPC에 쉽게 연결할 수 있도록 `ipc_client.h` 제공:

```c
/* cdp_client.h와 동일한 패턴: .h 파일에 static 함수 */
#include "ipc_client.h"

/* 연결 + 이름 등록 */
int fd = ipc_connect();
ipc_register(fd, "shell");

/* 특정 서비스에 메시지 전달 */
ipc_send(fd, "pkgmgr", "install", data, len);

/* 모든 클라이언트에 브로드캐스트 */
ipc_broadcast(fd, "shell", "app-launched", data, len);

/* 수신 메시지 처리 (콜백 기반) */
ipc_dispatch(fd, my_handler);
```

### 수정한 파일 (Class 20)

| 파일 | 설명 |
|------|------|
| `system/citc-ipc/src/ipc_proto.h` | 프로토콜 정의 (메시지 타입, 헤더, 페이로드) |
| `system/citc-ipc/src/ipc_daemon.c` | IPC 데몬 (~350줄) |
| `system/citc-ipc/src/ipc_client.h` | 헤더 전용 클라이언트 라이브러리 |
| `system/citc-ipc/Makefile` | 빌드 규칙 |
| `system/citcinit/services/citc-ipc.conf` | 서비스 설정 (소켓 활성화) |

### 문제와 해결 (Class 20)

**문제: `-Werror=format-truncation` 컴파일 에러**

```
src/ipc_daemon.c:287:27: error: '%s' directive output may be truncated
writing up to 323 bytes into a region of size 64 [-Werror=format-truncation=]
```

`snprintf`에서 소스 필드(프로토콜 구조체의 `char[64]`)를 대상 버퍼(역시 `char[64]`)에 복사할 때 발생. GCC가 `-Wformat-truncation`으로 소스 크기가 대상 크기를 초과할 **가능성**을 경고.

실제로 `ipc_send` 구조체의 `destination[64]`, `method[64]` 필드가 네트워크에서 수신한 데이터이므로, GCC가 페이로드 버퍼 전체 크기로 최대 문자열 길이를 추정.

**해결:** `%s` → `%.63s`로 precision 지정:

```c
/* 변경 전 - GCC 경고 */
snprintf(c->name, sizeof(c->name), "%s", reg->name);
snprintf(err.message, sizeof(err.message),
         "Service '%s' not found", msg->destination);

/* 변경 후 - 최대 63바이트만 출력하므로 truncation 없음 */
snprintf(c->name, sizeof(c->name), "%.63s", reg->name);
snprintf(err.message, sizeof(err.message),
         "Service '%.63s' not found", msg->destination);
```

`%.63s`는 문자열을 최대 63문자까지만 출력. 대상 버퍼 64바이트(63자 + NULL)에 정확히 맞으므로 GCC가 truncation 없음을 확인.

### 배운 점

- D-Bus의 핵심은 이름 등록 + 메시지 라우팅 + 브로드캐스트 (3가지)
- IPC 데몬도 poll() 이벤트 루프 패턴 사용 (init과 동일)
- 프로토콜 설계: 고정 크기 헤더 + 가변 페이로드가 파싱하기 쉬움
- serial 번호로 요청-응답을 매칭하는 것은 HTTP/2, gRPC 등에서도 동일한 패턴
- GCC의 `-Wformat-truncation`은 snprintf의 truncation 가능성을 정적 분석
- `%.Ns` precision 지정은 snprintf truncation 경고를 해결하는 정석

---

## Class 21: .desktop 파일 지원 + Phase 1 완료

> **핵심:** freedesktop.org의 .desktop 파일 표준을 구현하여, 하드코딩된 앱 버튼을 설정 파일 기반 동적 로드로 전환. Phase 1의 마지막 수업으로, 소켓 활성화 + IPC + 데스크탑 통합을 완성.

### 배경: .desktop 파일이란?

Linux 데스크탑 환경에서 앱 정보를 정의하는 표준 형식:

```
Windows:  .lnk 파일 (바로가기)     → 더블클릭 → 앱 실행
macOS:    .app/Info.plist          → Dock에 표시 → 앱 실행
Linux:    .desktop 파일            → 앱 메뉴/런처에 표시 → 앱 실행
```

**파일 위치:** `/usr/share/applications/`

**파일 형식 (INI 스타일):**

```ini
[Desktop Entry]
Name=Terminal
Exec=/usr/bin/citcterm
Icon=terminal
Type=Application
Categories=System;
```

**freedesktop.org 표준** — GNOME, KDE, XFCE 등 모든 Linux 데스크탑이 이 형식을 사용.

### 핵심 개념 1: .desktop 파서 설계

config.c의 설정 파서와 동일한 패턴:

```
한 줄씩 읽기 → '=' 찾기 → key/value 분리 → 키 매칭
```

```c
/* desktop_entry.h (header-only) */
struct desktop_entry {
    char name[64];    /* Name= 표시 이름 */
    char exec[256];   /* Exec= 실행 경로 */
    char icon[64];    /* Icon= 아이콘 (미래용) */
    int valid;        /* 파싱 성공 여부 */
};

/* 디렉토리 스캔 → .desktop 확장자 필터 → 파싱 */
int count = load_desktop_entries(entries, MAX_DESKTOP_ENTRIES);
```

**[Desktop Entry] 섹션 헤더 확인이 중요한 이유:**

.desktop 파일에는 여러 섹션이 있을 수 있음:

```ini
[Desktop Entry]        ← 이 섹션만 파싱
Name=Firefox
Exec=firefox

[Desktop Action NewWindow]  ← 다른 섹션은 무시
Name=New Window
Exec=firefox --new-window
```

### 핵심 개념 2: 동적 버튼 로드 + 폴백

기존 citcshell은 버튼이 하드코딩:

```c
/* 기존 (Class 17) - 하드코딩 */
void setup_buttons(void) {
    add_button("Terminal", "/usr/bin/citcterm");
    add_button("Demo", "/usr/bin/cdp_demo");
}
```

새로운 방식은 .desktop 파일에서 동적 로드하되, 파일이 없으면 폴백:

```c
/* 새로운 (Class 21) - .desktop 기반 + 폴백 */
static struct desktop_entry desktop_apps[MAX_DESKTOP_ENTRIES];

void setup_buttons(void) {
    int count = load_desktop_entries(desktop_apps, MAX_DESKTOP_ENTRIES);

    if (count > 0) {
        /* .desktop 파일에서 읽은 앱으로 버튼 생성 */
        for (int i = 0; i < count; i++)
            add_button(desktop_apps[i].name, desktop_apps[i].exec);
    } else {
        /* 폴백: 기존 하드코딩 */
        add_button("Terminal", "/usr/bin/citcterm");
        add_button("Demo", "/usr/bin/cdp_demo");
    }
}
```

**desktop_apps가 전역/정적이어야 하는 이유:**

```c
struct button {
    const char *label;  /* ← 포인터! 원본 문자열이 살아있어야 함 */
    const char *exec;
};

/* 만약 desktop_apps가 지역 변수라면: */
void setup_buttons(void) {
    struct desktop_entry apps[16];  /* ← 스택에 생성 */
    load_desktop_entries(apps, 16);
    add_button(apps[0].name, ...);  /* name 포인터 저장 */
}   /* ← 함수 끝 → apps 소멸 → dangling pointer! */

/* 해결: static으로 선언하여 프로그램 종료까지 유지 */
static struct desktop_entry desktop_apps[16];
```

### 수정한 파일 (Class 21)

| 파일 | 작업 |
|------|------|
| `display/shell/src/desktop_entry.h` | .desktop 파일 파서 (header-only) |
| `display/shell/src/citcshell.c` | setup_buttons()를 .desktop 기반으로 변경 |
| `display/shell/applications/terminal.desktop` | Terminal 앱 정의 |
| `display/shell/applications/demo.desktop` | Demo 앱 정의 |
| `tools/mkrootfs/build-initramfs.sh` | .desktop 복사 + citc-ipc 빌드 단계 |

### 문제와 해결 (Class 21)

**문제: 주석 안의 `/*`가 중첩 주석 경고 (`-Werror=comment`)**

```
src/citcshell.c:54:27: error: "/*" within comment [-Werror=comment]
   54 |  * /usr/share/applications/*.desktop 파일에서 앱 목록을 읽어
```

C 주석 `/* ... */` 안에서 `/*.desktop`의 `/*`가 중첩 주석 시작으로 인식됨:

```c
/*                              ← 주석 시작
 * /usr/share/applications/*.desktop  ← 여기서 /* 이 또 주석 시작?!
 */                             ← GCC 경고!
```

**해결:** 경로에서 `*`를 분리:

```c
/* 변경 전 */
 * /usr/share/applications/*.desktop 파일에서

/* 변경 후 */
 * /usr/share/applications/ 의 .desktop 파일에서
```

### 배운 점 (Class 21)

- .desktop 파일은 freedesktop.org의 앱 메타데이터 표준 — 모든 Linux 데스크탑이 사용
- 포인터를 저장하는 구조체에 문자열을 넘길 때 원본의 수명(lifetime) 관리가 핵심
- C 주석 안의 `/*`는 `-Wcomment`로 감지됨 — glob 패턴을 주석에 쓸 때 주의
- 하드코딩 → 설정 파일 기반으로의 전환 시 반드시 폴백(fallback)을 제공
- Phase 1 완성: init → 서비스 관리 → 네트워크 → 패키지 → 그래픽 → 데스크탑

---

## Class 22: 기존 코드 버그 수정 + 개선

**수정 파일:**

- `system/citcinit/src/service.c`
- `system/citcinit/src/socket_activation.c`
- `system/citc-ipc/src/ipc_daemon.c`
- `system/citc-ipc/src/ipc_client.h`
- `display/compositor/src/compositor.c`

### 핵심 개념 — 코드 리뷰

Phase 1 완료 후, 기존 코드를 체계적으로 검토하여 6개의 버그/개선점을 발견하고 수정함.
실제 프로덕션 환경에서 "작동하는 코드"와 "안전한 코드"는 다르다.

### 수정 사항

A-1. **CRITICAL** — service.c envp 컴팩션 루프 버그:

```c
/* 변경 전 (버그) — OR 조건으로 배열 범위를 넘어갈 수 있음 */
for (int i = 0; envp[i] != NULL || i < 5; i++) {

/* 변경 후 — 고정 크기 배열이므로 인덱스 기반 순회 */
for (int i = 0; i < 5; i++) {
```

A-2. **HIGH** — service.c dup2() 에러 체크 누락:

```c
/* 변경 전 */
dup2(svc->listen_fd, 3);

/* 변경 후 */
if (dup2(svc->listen_fd, 3) < 0)
    _exit(1);
```

`dup2()` 실패 시 자식 프로세스가 잘못된 fd로 계속 실행되면 데이터 손실 가능.

A-3. **HIGH** — ipc_daemon.c 부분 쓰기/읽기 미처리:

Non-blocking 소켓에서 `write()`/`read()`는 요청한 바이트보다 적게 처리할 수 있음:

```c
static ssize_t write_all(int fd, const void *buf, size_t len)
{
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, (const char *)buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -1;
        }
        written += n;
    }
    return (ssize_t)written;
}
```

`read_all()` 도 동일 패턴으로 구현. `send_msg()`와 `handle_client_message()`의 호출을 교체.

A-4. **MEDIUM** — socket_activation.c fcntl 에러 체크:

```c
if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    LOG_FAIL("fcntl O_NONBLOCK failed for '%s': %s",
             svc->name, strerror(errno));
    close(fd);
    unlink(svc->socket_path);
    continue;
}
```

A-5. **MEDIUM** — ipc_client.h 블로킹 WELCOME 읽기:

서버가 응답하지 않으면 클라이언트가 영원히 블로킹. `poll()` 타임아웃(2초) 추가:

```c
struct pollfd pfd = { .fd = fd, .events = POLLIN };
if (poll(&pfd, 1, 2000) > 0 && (pfd.revents & POLLIN)) {
    /* WELCOME 읽기 */
}
```

A-6. **LOW** — compositor.c LISTEN_FDS 유효성 검사 강화:

```c
int listen_fds_n = listen_fds_env ? atoi(listen_fds_env) : 0;
if (listen_fds_n > 0 && listen_fds_n <= 10) {  /* 범위 검증 추가 */
```

### 배운 점 (Class 22)

- `||` vs `&&` 루프 조건: 고정 크기 배열은 인덱스 기반이 안전
- 시스템 콜은 항상 반환값 확인 — 특히 `dup2()`, `fcntl()` 등
- Non-blocking I/O에서는 반드시 partial read/write 처리
- 네트워크 클라이언트는 타임아웃 없이 서버 응답을 기다리면 안 됨
- `atoi()` 결과는 범위 검증 필수 (음수, 과대값 거부)

---

## Class 23: NT 에뮬레이션 (ntdll + Object Manager)

**새 파일:**

- `wcl/include/stub_entry.h` — 공유 struct stub_entry 정의
- `wcl/src/ntemu/object_manager.h` — NT Object Manager API
- `wcl/src/ntemu/object_manager.c` — 스레드 안전 핸들 테이블
- `wcl/src/ntemu/ntdll.h` — NT Native API 선언
- `wcl/src/ntemu/ntdll.c` — NT Native API 구현

**수정 파일:**

- `wcl/src/dlls/kernel32/kernel32.c` — ntdll 호출로 리팩터
- `wcl/src/dlls/kernel32/kernel32.h` — stub_entry.h include로 변경
- `wcl/src/loader/citcrun.c` — 다중 DLL stub table 검색
- `wcl/src/loader/Makefile` — ntemu 소스 링크 추가
- `wcl/include/win32.h` — ERROR_INVALID_PARAMETER 추가

### 핵심 개념 — NT 계층 분리

Windows 아키텍처의 핵심 계층:

```
Win32 앱 (.exe)
    ↓  call CreateFileA()
kernel32.dll    ← Win32 인터페이스 (사용자 친화적)
    ↓  call NtCreateFile()
ntdll.dll       ← NT 네이티브 API (커널에 가까운 저수준)
    ↓  syscall
ntoskrnl.exe    ← 커널 모드
```

이 두 계층이 존재하는 이유:

- kernel32: 문자열 변환, 경로 정리, 에러 코드 변환 등 편의 기능
- ntdll: 커널과 1:1 대응하는 최소 인터페이스

**NTSTATUS vs Win32 에러 코드:**

| NTSTATUS | 값 | Win32 에러 | 값 |
| -------- | --- | --------- | --- |
| STATUS_SUCCESS | 0x00000000 | ERROR_SUCCESS | 0 |
| STATUS_OBJECT_NAME_NOT_FOUND | 0xC0000034 | ERROR_FILE_NOT_FOUND | 2 |
| STATUS_ACCESS_DENIED | 0xC0000022 | ERROR_ACCESS_DENIED | 5 |
| STATUS_INVALID_HANDLE | 0xC0000008 | ERROR_INVALID_HANDLE | 6 |

NTSTATUS는 bit 31이 severity (0=성공, 1=에러). `NT_SUCCESS(status)`는 `status >= 0` 확인.

**Object Manager:**
Windows NT 커널의 핵심 — 모든 커널 객체(파일, 프로세스, 뮤텍스 등)를 HANDLE로 추상화.
Linux의 fd 테이블과 유사하지만 더 범용적:

```c
struct ob_entry {
    enum ob_type type;   /* OB_FILE, OB_CONSOLE, OB_MUTEX, ... */
    int fd;              /* Linux file descriptor */
    uint32_t access;     /* 접근 권한 */
    void *extra;         /* 타입별 추가 데이터 */
};
```

`pthread_mutex_t`로 스레드 안전성 보장. 인덱스 0-2는 콘솔(stdin/stdout/stderr) 예약.

**다중 DLL stub table 검색:**

```c
static struct stub_entry *all_stub_tables[] = {
    kernel32_stub_table,
    ntdll_stub_table,
    advapi32_stub_table,
    NULL
};
```

새 DLL 추가 시 이 배열에 테이블만 추가하면 됨.

### 문제와 해결 (Class 23)

**문제 1: 정적 링크 시 `-lpthread` 순서 문제**

GCC 정적 링크에서 `-l` 플래그는 소스/오브젝트 파일 뒤에 와야 함:

```makefile
# 잘못됨 — pthread 심볼을 찾지 못함
$(CC) $(CFLAGS) -static -lpthread -o $@ $(SRCS)

# 올바름 — 소스 뒤에 라이브러리
$(CC) $(CFLAGS) -static -o $@ $(SRCS) -lpthread
```

**문제 2: `struct stub_entry` 불완전 타입 에러**

```
error: array type has incomplete element type 'struct stub_entry'
  164 | extern struct stub_entry ntdll_stub_table[];
```

ntdll.h와 registry.h에서 `struct stub_entry;` 전방 선언(forward declaration)을 사용했으나,
C 언어에서 `extern struct X array[]`는 **완전한 타입 정의**가 필요함.
(포인터 `struct X *ptr`는 전방 선언으로 충분하지만, 배열은 원소 크기를 알아야 하므로 불가.)

**해결:** 공유 헤더 `wcl/include/stub_entry.h` 생성:

```c
#ifndef CITC_STUB_ENTRY_H
#define CITC_STUB_ENTRY_H

struct stub_entry {
    const char *dll_name;
    const char *func_name;
    void *func_ptr;
};

#endif
```

kernel32.h, ntdll.h, registry.h 모두 이 헤더를 `#include`하도록 변경.
ntdll.c와 registry.c에서 kernel32.h를 include할 필요도 없어져 의존성이 깔끔해짐.

**교훈:** 여러 모듈이 공유하는 구조체는 독립 헤더로 분리하라.

### 배운 점 (Class 23)

- 실제 Windows 아키텍처를 이해하면 호환 레이어 설계가 명확해진다
- NTSTATUS → Win32 에러 코드 변환은 별도 계층에서 수행
- Object Manager 패턴은 다양한 커널 객체를 통합 관리하는 강력한 추상화
- 정적 링크 시 라이브러리 순서가 중요 (참조하는 쪽이 앞, 제공하는 쪽이 뒤)
- C에서 `extern struct X array[]`는 완전한 타입 정의가 필요 (전방 선언 불가)
- 여러 모듈이 공유하는 타입은 독립 헤더 파일로 분리하는 것이 좋은 관행
- 리팩터링은 기존 동작을 유지하면서 내부 구조만 변경해야 함

---

## Class 24: 레지스트리 v0.1 (파일 기반)

**새 파일:**

- `wcl/src/ntemu/registry.h` — 레지스트리 API + 상수
- `wcl/src/ntemu/registry.c` — 파일 기반 레지스트리 구현

**수정 파일:**

- `wcl/src/dlls/kernel32/kernel32.c` — reg_init() 호출 추가
- `wcl/src/loader/citcrun.c` — advapi32_stub_table 추가
- `wcl/src/loader/Makefile` — registry.c 링크 추가
- `tools/mkrootfs/build-initramfs.sh` — 레지스트리 디렉토리 생성

### 핵심 개념 — 레지스트리 구조

Windows 레지스트리란?

계층적 key-value 저장소. Linux의 `/etc/` 설정 파일에 해당하지만 단일 데이터베이스로 통합.

```
HKEY_LOCAL_MACHINE (HKLM)     ← 시스템 전역 설정
  └─ SOFTWARE
       └─ Microsoft
            └─ Windows
                 └─ CurrentVersion
  └─ SYSTEM
       └─ DriveMapping
            ├─ C = "/home/user/citc-c/"   ← REG_SZ 값
            └─ D = "/mnt/data/"
HKEY_CURRENT_USER (HKCU)      ← 사용자별 설정
```

**우리의 파일 기반 구현:**

```
/etc/citc-registry/
  HKLM/SYSTEM/DriveMapping/C    ← 파일 = 값
  HKCU/                         ← 디렉토리 = 키
```

값 파일 포맷: `[type:4B][length:4B][data:NB]`

**advapi32.dll:**
레지스트리 함수는 실제 Windows에서 advapi32.dll에서 export됨.
별도 DLL 테이블(`advapi32_stub_table`)로 구현하여 citcrun의 다중 DLL 검색에 통합.

**Win32 API와 내부 구현:**

| Win32 API | 내부 함수 | 동작 |
| --------- | -------- | ---- |
| RegOpenKeyExA | reg_open_key | 디렉토리 존재 확인 + 핸들 할당 |
| RegCreateKeyExA | reg_create_key | mkdir -p + 핸들 할당 |
| RegCloseKey | reg_close_key | 경로 문자열 해제 + 핸들 반환 |
| RegQueryValueExA | reg_query_value | 값 파일 읽기 |
| RegSetValueExA | reg_set_value | 값 파일 쓰기 |

### 문제와 해결 (Class 24)

**`-Werror=format-truncation` 경고 — registry.c snprintf**

```
error: '%s' directive output may be truncated writing likely 9 or more bytes
  379 | snprintf(value_path, sizeof(value_path), "%s/%s", path, name);
```

`path` (최대 1024바이트) + `/` + `name`을 결합하면 1024바이트 버퍼를 초과할 수 있음.
GCC `-Werror=format-truncation`이 컴파일 타임에 이를 감지.

**해결:** 정밀도 지정자로 각 인자의 최대 길이를 제한:

```c
/* 수정 전 — 잠재적 truncation */
snprintf(value_path, sizeof(value_path), "%s/%s", path, name);

/* 수정 후 — 768 + 1('/') + 254 = 1023 ≤ 1024 */
snprintf(value_path, sizeof(value_path), "%.768s/%.254s", path, name);
```

`reg_query_value()`와 `reg_set_value()` 두 곳 모두 동일 패턴 적용.
이 패턴은 Class 20 ipc_daemon.c에서 학습한 것과 동일한 해결법.

### 배운 점 (Class 24)

- 레지스트리 키 = 디렉토리, 값 = 파일로 매핑하면 디버깅이 쉬움
- Object Manager의 `extra` 필드를 활용하여 핸들에 경로 문자열 저장
- 루트 키(HKLM 등)는 특수 핸들값(0x80000000+)으로 구분하여 Object Manager 핸들과 혼동 방지
- `mkdir -p` 재귀 구현은 슬래시 단위로 잘라가며 mkdir 반복 호출
- `snprintf` format-truncation 경고는 `%.Ns` 정밀도 지정자로 해결 (Class 20과 동일 패턴)

---

## Class 25: kernel32 확장 + 다중 DLL 지원

**수정 파일:**

- `wcl/src/dlls/kernel32/kernel32.c` — 새 API 함수 추가
- `wcl/src/dlls/kernel32/kernel32.h` — kernel32_set_cmdline 선언
- `wcl/include/win32.h` — 메모리/힙/동기화 상수 추가
- `wcl/src/loader/citcrun.c` — 명령줄 설정

### 핵심 개념 — Win32 메모리와 환경 API

**Windows 메모리 관리 3계층:**

```
VirtualAlloc  ← 페이지 단위 (커널 직접, 대용량)  → mmap
HeapAlloc     ← 바이트 단위 (유저 모드 힙)       → malloc
malloc        ← C 런타임 (HeapAlloc 래퍼)        → malloc
```

**메모리 보호 플래그 매핑:**

| Windows | 값 | Linux (mmap) |
| ------- | --- | ----------- |
| PAGE_NOACCESS | 0x01 | PROT_NONE |
| PAGE_READONLY | 0x02 | PROT_READ |
| PAGE_READWRITE | 0x04 | PROT_READ + PROT_WRITE |
| PAGE_EXECUTE_READ | 0x20 | PROT_READ + PROT_EXEC |
| PAGE_EXECUTE_READWRITE | 0x40 | PROT_READ + PROT_WRITE + PROT_EXEC |

**추가된 함수 (13개):**

| 카테고리 | Win32 API | Linux 대응 |
| -------- | --------- | --------- |
| 메모리 | VirtualAlloc | mmap(MAP_ANONYMOUS) |
| 메모리 | VirtualFree | munmap |
| 힙 | GetProcessHeap | 가상 핸들 반환 |
| 힙 | HeapAlloc | malloc + memset |
| 힙 | HeapFree | free |
| 프로세스 | GetCurrentProcessId | getpid() |
| 프로세스 | GetCurrentThreadId | pthread_self() |
| 프로세스 | GetCurrentProcess | 가상 핸들 -1 |
| 환경 | GetEnvironmentVariableA | getenv() |
| 환경 | SetEnvironmentVariableA | setenv()/unsetenv() |
| 명령줄 | GetCommandLineA | 저장된 문자열 반환 |
| 모듈 | GetModuleHandleA | 고정 베이스 또는 NULL |
| 모듈 | GetModuleFileNameA | readlink("/proc/self/exe") |

총 kernel32 API: 11개(기존) + 13개(신규) = **24개**

### 배운 점 (Class 25)

- Windows의 메모리 관리는 3계층(Virtual/Heap/CRT)으로 분리되어 있다
- `GetCurrentProcess()`는 실제 핸들이 아닌 가상 핸들(-1)을 반환하는 설계
- 환경변수 API는 POSIX `getenv()`/`setenv()`와 거의 1:1 대응
- 기존 stub table 배열 기반 설계 덕분에 새 DLL 추가가 매우 쉬움
- 모든 ms_abi 함수는 Windows 호출 규약을 따라야 PE에서 정상 호출됨

---

## Class 26: Phase 2 테스트 + 디버깅

### 문제: file_test.exe segfault (si_addr=NULL)

**증상:** `citcrun file_test.exe` 실행 시 출력 없이 즉시 Segmentation fault.
hello.exe는 정상 동작.

**디버깅 과정:**

1. `strace`로 추적 → `si_addr=NULL` (NULL 포인터 역참조) 확인
2. `x86_64-w64-mingw32-objdump -d` 로 PE 디스어셈블리 확인
3. 엔트리포인트(RVA=0x1000)의 코드가 `_start`가 아닌 `print` 함수임을 발견

**근본 원인:**

MinGW 링커의 기본 엔트리포인트는 `mainCRTStartup` (CRT 시작 함수).
`-nostdlib`에서는 CRT가 없으므로 링커가 `.text` 섹션의 시작 주소를 엔트리포인트로 사용.

file_test.c에서 `print()`와 `print_num()`이 `_start()`보다 위에 정의되어 있어
컴파일러가 이들을 `.text` 앞쪽에 배치:

```text
.text 시작 (RVA 0x1000) → print()      ← 엔트리포인트가 여기를 가리킴!
                         → print_num()
                         → _start()     ← 실제 진입점은 여기
```

citcrun이 `entry()` (인수 없음)를 호출하면 `print(HANDLE, const char*)`가
실행되어 RCX/RDX의 쓰레기 값을 포인터로 사용 → NULL 역참조 → segfault.

hello_win.c는 `_start`만 있어서 `.text` 시작 = `_start` → 우연히 동작했음.

**해결:**

```makefile
# 수정 전
CROSS_LDFLAGS = -lkernel32

# 수정 후 — 엔트리포인트를 _start로 명시 지정
CROSS_LDFLAGS = -lkernel32 -Wl,-e,_start
```

**검증:** `RVA=0x1144` (`_start`)로 엔트리포인트가 올바르게 변경됨. 9개 테스트 모두 통과.

### 부수 발견: last_error 덮어쓰기

테스트 [9]에서 `GetLastError()`가 2(ERROR_FILE_NOT_FOUND) 대신 0을 반환.

원인: `CreateFileA` 실패 후 `print()` → `WriteFile` 성공 → `last_error = 0`으로 덮어씀.
실제 Windows에서는 API 성공 시 last_error를 변경하지 않음.
우리 구현은 모든 API에서 `last_error = nt_status_to_win32(status)`를 무조건 설정.

추후 개선: 성공 시 `last_error`를 건드리지 않도록 수정.

### 배운 점 (Class 26)

- `-nostdlib` 사용 시 반드시 `-Wl,-e,_start`로 엔트리포인트를 명시해야 함
- `strace`의 `si_addr` 값으로 크래시 유형 파악 가능 (NULL=포인터 역참조)
- `objdump -d`로 PE 디스어셈블리를 확인하면 엔트리포인트의 실제 코드를 알 수 있음
- 함수 정의 순서가 바이너리 배치에 영향을 줄 수 있음 (C 파일 내 순서 = .text 내 순서)
- 실제 Windows의 `SetLastError` 동작: 실패 시만 설정, 성공 시 기존 값 유지

## Class 27: last_error 버그 수정 + API 테스트 확장

### 버그 수정: last_error 성공 시 덮어쓰기

**문제:**

kernel32.c의 모든 Win32 API 함수가 성공/실패에 관계없이 `last_error`를 설정.
성공 시 `last_error = ERROR_SUCCESS(0)`으로 덮어쓰여, 이전에 설정된 에러 코드가 사라짐.

```c
/* 수정 전 (버그) — 성공 시에도 덮어씀 */
last_error = nt_status_to_win32(status);
return NT_SUCCESS(status) ? TRUE : FALSE;

/* 수정 후 — 실패 시에만 설정 */
if (!NT_SUCCESS(status)) {
    last_error = nt_status_to_win32(status);
    return FALSE;
}
return TRUE;
```

**영향 범위 (14개 함수):**

| 패턴 | 함수 |
|------|------|
| `nt_status_to_win32` 무조건 | CreateFileA, WriteFile, ReadFile, CloseHandle, DeleteFileA |
| `ERROR_SUCCESS` 불필요 설정 | GetFileSize, SetFilePointer, VirtualAlloc, VirtualFree, HeapAlloc, HeapFree, GetEnvironmentVariableA, SetEnvironmentVariableA, GetModuleFileNameA |

**검증:**

file_test.exe [9]: DeleteFileA 후 CreateFileA(OPEN_EXISTING) 실패 → GetLastError()가
이제 `error=2` (ERROR_FILE_NOT_FOUND) 정상 반환. (수정 전: `error=0`)

### 새 테스트 프로그램

**api_test.c** — Class 25 kernel32 API 10개 테스트:

| # | API | 검증 내용 |
|---|-----|----------|
| 1 | VirtualAlloc | 4096 바이트 할당 + 읽기/쓰기 |
| 2 | VirtualFree | 해제 성공 |
| 3 | GetProcessHeap | 비-NULL 반환 |
| 4 | HeapAlloc | 256 바이트 + HEAP_ZERO_MEMORY 확인 |
| 5 | HeapFree | 해제 성공 |
| 6 | SetEnvironmentVariableA | "CITC_TEST"="hello" 설정 |
| 7 | GetEnvironmentVariableA | "hello" 읽기 확인 |
| 8 | GetCommandLineA | 비어있지 않은 문자열 반환 |
| 9 | GetCurrentProcessId | pid > 0 확인 |
| 10 | GetModuleHandleA(NULL) | 비-NULL 반환 |

**reg_test.c** — Class 24 레지스트리 API 6개 테스트:

| # | API | 검증 내용 |
|---|-----|----------|
| 1 | RegCreateKeyExA | HKLM\SOFTWARE\CitcTest 키 생성 |
| 2 | RegSetValueExA | REG_SZ "Hello Registry!" 쓰기 |
| 3 | RegSetValueExA | REG_DWORD 42 쓰기 |
| 4 | RegQueryValueExA | REG_SZ 읽기 + 값 비교 |
| 5 | RegQueryValueExA | REG_DWORD 읽기 + 값=42 확인 |
| 6 | RegCloseKey | 키 핸들 닫기 |

### 테스트 결과

| 테스트 | 결과 | 비고 |
|--------|------|------|
| hello.exe | PASS | 기본 출력 |
| file_test.exe | 9/9 PASS | last_error 수정 확인 |
| api_test.exe | 10/10 PASS | 메모리, 환경변수, 프로세스 API 전부 동작 |
| reg_test.exe | 6/6 PASS | 레지스트리 경로 동적 결정 후 통과 |

### 수정한 파일

| 파일 | 내용 |
|------|------|
| `wcl/src/dlls/kernel32/kernel32.c` | 14개 함수에서 성공 시 last_error 설정 제거 |
| `wcl/src/ntemu/registry.c` | 레지스트리 기본 경로를 런타임 결정 (getuid, $HOME) |
| `wcl/src/ntemu/registry.h` | `REGISTRY_BASE_PATH` → `reg_get_base_path()` 함수로 변경 |

### 새 파일

| 파일 | 내용 |
|------|------|
| `wcl/tests/api_test.c` | kernel32 메모리/환경/프로세스 API 테스트 (10개) |
| `wcl/tests/reg_test.c` | advapi32 레지스트리 API 테스트 (6개) |

### 배운 점 (Class 27)

- Windows 규칙: API 성공 시 `SetLastError`를 호출하지 않음 (기존 에러 코드 보존)
- 테스트에서 `GetLastError()` 검증은 중간에 다른 API 호출이 끼면 깨질 수 있음 — 에러 코드는 실패 직후 즉시 확인해야 함
- advapi32.dll 함수(Reg*)를 사용하는 테스트는 MinGW에서 `-ladvapi32` 추가 필요
- 레지스트리 경로를 동적으로 결정해야 함: root이면 `/etc/citc-registry`, 일반 유저면 `$HOME/.citc-registry`
- `#define`으로 정의한 경로를 함수 반환값으로 바꿀 때, `MACRO "/suffix"` 같은 문자열 리터럴 연결은 함수 호출에서는 불가 → `snprintf`로 교체 필요
- `getuid() == 0`으로 root 여부 확인, `$CITC_REGISTRY_PATH` 환경변수로 오버라이드 가능하게 설계

---

## Class 28: 윈도우 관리 + GDI 렌더링 (user32 + gdi32)

> Phase 3 시작: Win32 GUI 프로그램 실행.
> user32.dll(윈도우 관리, 메시지 루프)과 gdi32.dll(2D 그래픽 렌더링) 구현.

### 핵심 개념

**Win32 GUI 아키텍처:**
```
앱 → RegisterClassA(WndProc 등록)
    → CreateWindowExA(HWND 생성 + CDP surface 매핑)
    → ShowWindow + UpdateWindow
    → GetMessageA 루프:
        poll(CDP소켓, self-pipe) → 이벤트 대기
        TranslateMessage → WM_KEYDOWN → WM_CHAR 변환
        DispatchMessageA → WndProc 콜백 호출
    → WM_PAINT:
        BeginPaint(HDC 할당 + 배경 지우기)
        TextOutA(8x8 비트맵 폰트 렌더링)
        EndPaint(HDC 해제 + CDP commit)
    → WM_CLOSE → DestroyWindow → WM_DESTROY → PostQuitMessage
    → GetMessageA returns FALSE → 루프 종료
```

**핸들 네임스페이스 분리:**

| 핸들 종류 | 오프셋 | 범위 |
|-----------|--------|------|
| NT HANDLE (파일, 뮤텍스 등) | 0x100 | Object Manager |
| HWND (윈도우) | 0x10000 | user32 wnd_table |
| HDC (Device Context) | 0x20000 | gdi32 dc_table |
| HGDIOBJ (브러시 등) | 0x30000 | gdi32 gdi_obj_table |

**CDP 통합 — HWND ↔ CDP surface 매핑:**
- `CreateWindowExA` → `cdp_create_surface()` → 공유메모리 픽셀 버퍼 획득
- `EndPaint` → `cdp_commit_to()` → 컴포지터에 렌더링 완료 알림
- CDP 이벤트 콜백 → Win32 메시지로 변환 → 큐에 추가
- 컴포지터 미실행 시: 로컬 `mmap` 픽셀 버퍼 + self-pipe만으로 동작

**GetMessageA 블로킹 전략:**
```
poll() on:
  [0] g_cdp->sock_fd  — CDP 이벤트 (키보드, 마우스, 포커스)
  [1] msg_pipe[0]     — PostMessage/PostQuitMessage 깨우기

우선순위:
  1. WM_QUIT 체크 → FALSE 반환 (루프 종료)
  2. 큐에 메시지 → 꺼내서 반환
  3. needs_paint 윈도우 → WM_PAINT 생성 (최저 우선순위)
  4. poll() 블로킹 → CDP 이벤트 또는 PostMessage 대기
```

**GDI Device Context (HDC) 상태 머신:**

| 상태 | 설정 함수 | 기본값 |
|------|----------|--------|
| text_color | SetTextColor | RGB(0,0,0) 검정 |
| bk_color | SetBkColor | RGB(255,255,255) 흰색 |
| bk_mode | SetBkMode | OPAQUE |
| brush_color | SelectObject | RGB(255,255,255) 흰색 |

### ABI 주의사항

**LONG 타입 크기 차이 (수정됨):**
- Windows x64 (LLP64): `long` = 4바이트
- Linux x64 (LP64): `long` = 8바이트
- `typedef long LONG;` → `typedef int32_t LONG;`으로 수정
- 이 차이가 RECT, PAINTSTRUCT 등 구조체 레이아웃을 깨뜨림

**Lazy CDP 초기화:**
- `user32_init()`: 테이블 + self-pipe만 초기화 (CDP 연결 안 함)
- `CreateWindowExA` 첫 호출 시 `cdp_connect()` 시도
- 콘솔 앱은 CDP 연결 타임아웃(2.5초)을 피할 수 있음

### 구현한 함수

**user32.dll (18개):**

| 함수 | 역할 |
|------|------|
| RegisterClassA | WNDCLASS 등록, atom 반환 |
| CreateWindowExA | HWND 할당 + CDP surface + WM_CREATE |
| DestroyWindow | WM_DESTROY + surface 해제 |
| ShowWindow | visible 설정 + needs_paint |
| UpdateWindow | WM_PAINT 동기 전송 |
| GetMessageA | poll() 블로킹 메시지 루프 |
| TranslateMessage | WM_KEYDOWN → WM_CHAR 변환 |
| DispatchMessageA | WndProc 콜백 호출 |
| PostQuitMessage | quit_posted 플래그 설정 |
| DefWindowProcA | 기본 메시지 처리 (WM_CLOSE→DestroyWindow) |
| PostMessageA | 큐에 추가 + self-pipe 깨우기 |
| SendMessageA | WndProc 직접 호출 (동기) |
| BeginPaint | HDC 할당 + 배경 지우기 |
| EndPaint | HDC 해제 + CDP commit |
| GetClientRect | {0, 0, width, height} 반환 |
| InvalidateRect | needs_paint 설정 |
| MessageBoxA | stderr 출력 스텁 |
| FillRect | 사각형 채우기 (gdi32에서 구현) |

**gdi32.dll (13개):**

| 함수 | 역할 |
|------|------|
| GetDC / ReleaseDC | DC 할당/해제 |
| TextOutA | font8x8 비트맵 폰트로 텍스트 렌더링 |
| SetPixel / GetPixel | 단일 픽셀 읽기/쓰기 |
| Rectangle | 사각형 테두리 그리기 |
| CreateSolidBrush | HBRUSH 생성 |
| DeleteObject | GDI 오브젝트 해제 |
| SelectObject | DC에 브러시 설정 |
| SetTextColor | 텍스트 색상 설정 |
| SetBkColor | 배경 색상 설정 |
| SetBkMode | TRANSPARENT/OPAQUE 모드 |

### 테스트 결과

| 테스트 | 결과 | 비고 |
|--------|------|------|
| hello.exe | PASS | 회귀 테스트 |
| api_test.exe | 10/10 PASS | 회귀 테스트 |
| gui_test.exe | 9/9 PASS | 전체 GUI 생명주기 (WSL, 컴포지터 없음) |

**gui_test.exe 검증 항목:**

| # | 항목 | 내용 |
|---|------|------|
| 1 | RegisterClassA | atom=1 반환 |
| 2 | CreateWindowExA | HWND=0x10000 + WM_CREATE 전달 |
| 3 | GetClientRect | 400x300 확인 |
| 4 | ShowWindow | visible 설정 |
| 5 | UpdateWindow | WM_PAINT 동기 전송 |
| 6 | WM_PAINT | BeginPaint + TextOutA + EndPaint |
| 7 | WM_CREATE | WndProc 콜백 정상 호출 |
| 8 | WM_DESTROY | DestroyWindow → PostQuitMessage |
| 9 | Message loop | GetMessageA FALSE 반환으로 루프 종료 |

### 새 파일

| 파일 | 설명 |
|------|------|
| `wcl/src/dlls/user32/user32.h` | user32 헤더 (init, stub_table) |
| `wcl/src/dlls/user32/user32.c` | user32 구현 (~600줄, HWND + 메시지 루프 + CDP 통합) |
| `wcl/src/dlls/gdi32/gdi32.h` | gdi32 헤더 (stub_table, 내부 API) |
| `wcl/src/dlls/gdi32/gdi32.c` | gdi32 구현 (~490줄, HDC + TextOutA + 도형) |
| `wcl/tests/gui_test.c` | Win32 GUI 통합 테스트 (9개 검증) |

### 수정한 파일

| 파일 | 내용 |
|------|------|
| `wcl/include/win32.h` | GUI 타입/상수 추가 + LONG을 int32_t로 수정 |
| `wcl/src/loader/citcrun.c` | user32/gdi32 stub table + user32_init() 추가 |
| `wcl/src/loader/Makefile` | user32.c, gdi32.c를 SRCS에 추가 |
| `wcl/tests/Makefile` | gui_test.exe 빌드 규칙 추가 |

### 배운 점 (Class 28)

- **LLP64 vs LP64**: Windows x64에서 `long`은 4바이트, Linux x64에서는 8바이트. ABI 호환을 위해 `int32_t`나 `int`를 사용해야 함
- **Self-pipe trick**: `poll()`로 블로킹하면서 내부 이벤트(PostMessage)로 깨어나게 하려면 `pipe()` + non-blocking write로 구현
- **Lazy 초기화**: GUI 관련 리소스(CDP 연결)를 실제 사용 시점에 초기화하면 콘솔 앱의 성능 페널티를 피할 수 있음
- **Header-only 라이브러리 경고 억제**: `#pragma GCC diagnostic push/ignored/pop`으로 사용하지 않는 static 함수 경고를 파일 단위로 억제
- **HWND/HDC/HGDIOBJ 네임스페이스**: 서로 다른 핸들 타입은 오프셋을 분리하여 충돌 방지. NT HANDLE(0x100), HWND(0x10000), HDC(0x20000), HGDI(0x30000)
- **CDP 이벤트 → Win32 메시지 변환**: 콜백 패턴으로 변환. CDP의 surface_id로 HWND를 역매핑, 포커스 윈도우 추적으로 키 이벤트 라우팅
- **WM_PAINT 최저 우선순위**: 실제 Windows와 동일하게, 큐가 비었을 때만 WM_PAINT를 생성. 다른 메시지가 먼저 처리됨

## Class 30: 입력 통합 + QEMU 실제 테스트

> Phase 3 완성: 키보드 입력 변환 (Linux evdev → Windows VK) 구현 + QEMU에서 컴포지터 연동 실제 GUI 테스트.

### 핵심 개념

**Linux evdev keycode vs Windows Virtual Key (VK):**

두 시스템은 키보드를 완전히 다른 번호 체계로 표현한다.

| 키 | Linux evdev (KEY_*) | Windows VK (VK_*) |
| ---- | -------------------- | -------------------- |
| A | 30 (KEY_A) | 0x41 ('A') |
| Enter | 28 (KEY_ENTER) | 0x0D (VK_RETURN) |
| Escape | 1 (KEY_ESC) | 0x1B (VK_ESCAPE) |
| Space | 57 (KEY_SPACE) | 0x20 (VK_SPACE) |
| F1 | 59 (KEY_F1) | 0x70 (VK_F1) |
| Left Ctrl | 29 (KEY_LEFTCTRL) | 0xA2 (VK_LCONTROL) |

- evdev: 하드웨어 스캔코드 기반 (물리적 키 위치)
- VK: 논리적 키 의미 기반 (문자/기능)
- 변환 테이블: 128개 엔트리 배열 (인덱스 = evdev, 값 = VK)

**WM_KEYDOWN/WM_KEYUP lParam 구조:**
```
bit 0-15:   repeat count (보통 1)
bit 16-23:  scan code (하드웨어 스캔코드)
bit 24:     extended key flag (화살표, Numpad Enter 등)
bit 29:     context code (Alt 키 상태)
bit 30:     previous key state (이전에 눌려 있었으면 1)
bit 31:     transition state (0=눌림, 1=해제)
```

Phase 3에서는 repeat count(1) + scan code(evdev keycode) + state flags(bit 30-31)만 설정.

**TranslateMessage — WM_CHAR 생성:**

```
WM_KEYDOWN(VK_A) → TranslateMessage() → WM_CHAR('a') 큐에 추가

문자 결정 우선순위:
  1. CDP가 보낸 ASCII character (g_last_char)
  2. VK 코드에서 추론 (A-Z → a-z, 0-9, Space, Enter 등)
```

Win32에서 문자 입력은 항상 WM_KEYDOWN → WM_CHAR 2단계.
WM_KEYDOWN은 물리적 키(VK), WM_CHAR는 논리적 문자(ASCII/Unicode).

### 구현 상세 (Class 30)

**keymap.h (변환 테이블):**

```c
/* 배열 인덱스 = Linux evdev keycode, 값 = Windows VK 코드 */
static const uint8_t evdev_to_vk[128] = {
    [1]  = VK_ESCAPE,     /* KEY_ESC */
    [2]  = '1',           /* KEY_1 → VK_1 */
    ...
    [30] = 'A',           /* KEY_A → VK_A */
    ...
    [57] = VK_SPACE,      /* KEY_SPACE */
    [103] = VK_UP,        /* KEY_UP → 화살표 ↑ */
    ...
};

static inline uint32_t linux_keycode_to_vk(uint32_t keycode) {
    if (keycode >= 128) return 0;
    return evdev_to_vk[keycode];
}
```

매핑 범위: 알파벳(A-Z), 숫자(0-9), 펑션키(F1-F12), 넘패드(0-9, +-*/.), 특수키(Esc, Tab, Enter, Backspace, Space), 탐색키(화살표, Home, End, PgUp, PgDn, Insert, Delete), 수식키(Shift, Ctrl, Alt, Win), 구두점(;:, =+, ,<, -_, .>, /?, `~ 등).

**on_cdp_key 변경 (user32.c):**

| 항목 | 이전 | 이후 |
| ------ | ------ | ------ |
| wParam | raw evdev keycode | `linux_keycode_to_vk(keycode)` |
| lParam | ASCII char | repeat(1) + scancode(bit 16-23) + state(bit 30-31) |
| 미지원 키 | 그대로 전달 | `vk == 0`이면 무시 |
| 문자 전달 | lParam에 직접 | `g_last_char`에 저장 → TranslateMessage에서 사용 |

### QEMU 실제 테스트 (컴포지터 연동)

WSL에서의 테스트(컴포지터 없음)는 Class 28에서 완료.
이번에는 **QEMU GUI 모드에서 컴포지터를 실행**하여 실제 화면에 윈도우가 표시되는지 검증.

**테스트 절차:**

```bash
# 1. initramfs 재빌드 (gui_test.exe 포함)
bash tools/mkrootfs/build-initramfs.sh

# 2. QEMU GUI 모드 부팅
bash tools/run-qemu.sh --gui

# 3. 시리얼 콘솔에서 (부팅 완료 후)
compositor &          # 컴포지터 백그라운드 실행
sleep 1               # 소켓 준비 대기
citcrun /opt/wcl-tests/gui_test.exe   # GUI 테스트 실행
```

**결과:**

- QEMU 윈도우에 "GUI Test Window" 타이틀 윈도우 표시
- 윈도우 안에 "Hello Win32 GUI!" 텍스트 렌더링 확인
- 자동으로 WM_PAINT → PostMessage(WM_CLOSE) → WM_DESTROY → 종료
- 시리얼 콘솔 출력: **9/9 PASS**

**전체 테스트 결과 (QEMU):**

| 테스트 | 결과 | 환경 |
|--------|------|------|
| hello.exe | PASS | QEMU + 컴포지터 |
| api_test.exe | 10/10 PASS | QEMU + 컴포지터 |
| gui_test.exe | 9/9 PASS | QEMU + 컴포지터 (실제 윈도우 표시) |

### build-initramfs.sh 수정

테스트 .exe 복사 로직을 개별 파일 지정에서 glob 패턴으로 변경:

```bash
# 이전: hello.exe, file_test.exe만 개별 복사
# 이후: build/*.exe 전체 복사
for exe in "${PROJECT_ROOT}/wcl/tests/build/"*.exe; do
    cp "$exe" "${INITRAMFS_DIR}/opt/wcl-tests/"
done
```

새 테스트 .exe가 추가되면 자동으로 initramfs에 포함.

### 새 파일

| 파일 | 설명 |
| ------ | ------ |
| `wcl/src/dlls/user32/keymap.h` | Linux evdev → Windows VK 변환 테이블 (128개 키, ~280줄) |

### 수정한 파일

| 파일 | 내용 |
| ------ | ------ |
| `wcl/src/dlls/user32/user32.c` | keymap.h 적용: on_cdp_key VK 변환, TranslateMessage g_last_char 패턴 |
| `tools/mkrootfs/build-initramfs.sh` | 테스트 .exe glob 복사로 변경 (gui_test.exe 등 자동 포함) |

### 배운 점 (Class 30)

- **evdev → VK 변환은 단순 배열 룩업**: 128개 엔트리 배열로 O(1) 변환. 범위 밖은 0 반환으로 미지원 키 자연 무시
- **WM_KEYDOWN과 WM_CHAR의 분리**: Win32에서 키 입력은 2단계 — 물리적 키(VK)와 논리적 문자(char)가 별도 메시지. TranslateMessage가 이 변환을 담당
- **lParam 비트 필드 설계**: Win32의 lParam은 30년 전 16비트 시대의 유산이지만, 현대 앱도 스캔코드와 상태 플래그를 읽음. 올바른 구성이 호환성에 중요
- **initramfs 빌드 스크립트에서 glob 패턴 사용**: 개별 파일 지정 대신 `*.exe` glob으로 새 테스트 추가 시 스크립트 수정 불필요
- **QEMU GUI 테스트의 가치**: WSL 로컬 테스트(컴포지터 없음)로 로직 검증 → QEMU 실제 테스트로 렌더링 검증. 두 단계 분리가 디버깅 효율을 높임

### Phase 3 완료 요약

Phase 3 (Class 28-30)에서 달성한 것:

| 기능 | 상태 |
|------|------|
| user32.dll (18개 API) | 구현 완료 |
| gdi32.dll (13개 API) | 구현 완료 |
| HWND ↔ CDP surface 매핑 | 동작 확인 |
| Win32 메시지 루프 (GetMessageA/DispatchMessageA) | 동작 확인 |
| 키보드 입력 변환 (evdev → VK) | 구현 완료 |
| TextOutA 텍스트 렌더링 | 동작 확인 (QEMU에서 화면 표시) |
| WSL 로컬 테스트 | 9/9 PASS |
| QEMU 실제 테스트 (컴포지터 연동) | 9/9 PASS |

**Phase 3 검증 완료: Win32 GUI 프로그램이 CITC OS에서 실행되어 실제 화면에 윈도우가 표시됨.**

---

## Class 31: user32/gdi32 확장 (Phase 3 보강)

> Phase 3 기본 완료 후 보강: 타이머, 윈도우 속성, 시스템 정보, 스톡 오브젝트, DrawText 등 실제 Win32 앱에 필요한 API 18개 추가.

### 핵심 개념

**SetTimer / KillTimer — Win32 타이머 시스템:**

```
앱: SetTimer(hwnd, id=1, 100ms, NULL)
→ 타이머 테이블에 등록
→ GetMessageA의 poll() 루프에서 매 반복마다 체크
→ 100ms 경과 시 WM_TIMER 메시지 큐에 추가
→ DispatchMessageA → WndProc(hwnd, WM_TIMER, 1, 0)

우선순위: 일반 메시지 > WM_TIMER > WM_PAINT
(실제 Windows와 동일한 우선순위)
```

시간 소스: `clock_gettime(CLOCK_MONOTONIC)` → 밀리초 변환.
poll() 타임아웃을 가장 가까운 타이머까지 남은 시간으로 설정하여 정확한 타이밍 보장.

**GetWindowLongA / SetWindowLongA — 윈도우 속성 접근:**

```
인덱스 값이 음수인 이유:
  양수 (0, 4, 8...): cbWndExtra 영역 접근 (앱 정의 바이트)
  음수: 시스템 속성

GWL_WNDPROC  (-4):  윈도우 프로시저 (서브클래싱의 핵심)
GWL_STYLE    (-16): 윈도우 스타일 (WS_OVERLAPPEDWINDOW 등)
GWL_EXSTYLE  (-20): 확장 스타일
GWLP_USERDATA(-21): 앱 전용 데이터 (C++에서 this 포인터 저장용)
```

**서브클래싱 패턴:**

```c
// 기존 WndProc 저장
WNDPROC oldProc = (WNDPROC)GetWindowLongA(hwnd, GWL_WNDPROC);
// 새 WndProc으로 교체
SetWindowLongA(hwnd, GWL_WNDPROC, (LONG)newProc);
// newProc 안에서 처리 안 할 메시지는 oldProc에 전달
```

**GetStockObject — 시스템 GDI 오브젝트:**

```
스톡 오브젝트 = 시스템이 미리 만들어놓은 GDI 오브젝트.
앱이 CreateSolidBrush/DeleteObject 할 필요 없음.

GetStockObject(WHITE_BRUSH)  → 흰색 브러시
GetStockObject(BLACK_PEN)    → 검정 펜
GetStockObject(SYSTEM_FONT)  → 시스템 기본 폰트

내부 구현:
  HGDIOBJ = (void*)(STOCK_OFFSET + index)
  STOCK_OFFSET = 0x40000 (HGDI_OFFSET=0x30000와 범위 분리)
  SelectObject에서 이 범위를 인식하여 스톡 테이블 참조.
  DeleteObject에서 이 범위를 무시 (시스템 소유이므로 삭제 불가).
```

**DrawTextA — 포맷 텍스트 출력:**

TextOutA의 확장판. RECT 안에 정렬하여 텍스트를 출력.

```
플래그 조합 예:
  DT_CENTER | DT_VCENTER | DT_SINGLELINE  → 사각형 정중앙
  DT_CALCRECT                              → 그리지 않고 필요 크기만 계산

DT_CALCRECT 용도:
  1단계: DT_CALCRECT로 텍스트 크기 계산
  2단계: 그 크기로 실제 배치/렌더링
  → 동적 레이아웃 구현에 필수
```

**GetTextMetricsA — 폰트 메트릭스:**

```
TEXTMETRICA 구조체의 핵심 필드:
  tmHeight = 8       (font8x8: 8픽셀 높이)
  tmAveCharWidth = 8 (고정폭: 모든 글자 8픽셀)
  tmAscent = 7       (기준선 위 높이)
  tmDescent = 1      (기준선 아래 깊이)

실제 Windows: TrueType 래스터라이저가 폰트별 정확한 메트릭스 반환.
Phase 3: font8x8 고정폭 비트맵 폰트의 고정 메트릭스.
```

### 구현 상세 (Class 31)

**타이머 인프라 (user32.c):**

```c
#define MAX_TIMERS 32

struct timer_entry {
    int active;
    HWND hwnd;
    uintptr_t timer_id;
    UINT interval_ms;
    uint64_t next_fire_ms;   /* clock_gettime(CLOCK_MONOTONIC) 밀리초 */
};

/* GetMessageA에서의 통합:
 * 1. 큐에 메시지 있으면 바로 반환
 * 2. 타이머 체크 → 만료된 타이머가 있으면 WM_TIMER 큐에 추가
 * 3. WM_PAINT 체크 (최저 우선순위)
 * 4. poll() 대기 — timeout = min(100ms, 가장 가까운 타이머)
 */
```

**스톡 오브젝트 테이블 (gdi32.c):**

```c
#define STOCK_OFFSET 0x40000

static struct {
    int type;        /* STOCK_BRUSH=1, STOCK_PEN=2, STOCK_FONT=3 */
    COLORREF color;
} stock_objects[20] = {
    [WHITE_BRUSH]      = { 1, 0x00FFFFFF },
    [BLACK_BRUSH]      = { 1, 0x00000000 },
    [SYSTEM_FONT]      = { 3, 0x00000000 },
    [DEFAULT_GUI_FONT] = { 3, 0x00000000 },
    /* ... */
};
```

**wnd_entry 확장:**

```c
struct wnd_entry {
    /* 기존 필드 ... */
    DWORD ex_style;       /* GWL_EXSTYLE용 (새 필드) */
    uintptr_t user_data;  /* GWLP_USERDATA용 (새 필드) */
};
```

### 추가된 API (18개)

**user32.dll (15개):**

| 함수 | 설명 | 핵심 구현 |
| ---- | ---- | -------- |
| SetTimer | 타이머 등록 | timer_table + GetMessageA poll() 연동 |
| KillTimer | 타이머 해제 | timer_table에서 비활성화 |
| GetWindowLongA | 윈도우 속성 조회 | switch(index) → wnd_entry 필드 |
| SetWindowLongA | 윈도우 속성 설정 | switch(index) → wnd_entry 필드 |
| IsWindow | HWND 유효성 확인 | hwnd_to_wnd() != NULL |
| IsWindowVisible | 표시 상태 확인 | w->visible |
| GetWindowRect | 스크린 좌표 사각형 | x,y,x+w,y+h |
| SetWindowTextA | 타이틀 변경 | strncpy(w->title) |
| GetWindowTextA | 타이틀 조회 | memcpy(buf, w->title) |
| MoveWindow | 위치/크기 변경 | x,y,w,h 갱신 + repaint 플래그 |
| SetFocus | 포커스 변경 | g_focus_hwnd 설정 |
| GetFocus | 포커스 조회 | g_focus_hwnd 반환 |
| GetSystemMetrics | 시스템 정보 | switch(index) 하드코딩 |
| LoadCursorA | 커서 로드 스텁 | 더미 핸들 0xCCCC0001 |
| LoadIconA | 아이콘 로드 스텁 | 더미 핸들 0xCCCC0002 |

**gdi32.dll (3개):**

| 함수 | 설명 | 핵심 구현 |
| ---- | ---- | -------- |
| GetStockObject | 스톡 오브젝트 | STOCK_OFFSET + index 반환 |
| DrawTextA | 포맷 텍스트 | TextOutA + 정렬 계산 + DT_CALCRECT |
| GetTextMetricsA | 폰트 메트릭스 | font8x8 고정값 반환 |

### win32.h 추가 상수

| 범주 | 추가된 상수 |
| ---- | ---------- |
| GetWindowLong | GWL_WNDPROC(-4), GWL_STYLE(-16), GWL_EXSTYLE(-20), GWLP_USERDATA(-21) |
| GetSystemMetrics | SM_CXSCREEN(0), SM_CYSCREEN(1), SM_CXICON(11), SM_CYICON(12), SM_CXCURSOR(13), SM_CYCURSOR(14) |
| DrawText | DT_CENTER, DT_RIGHT, DT_VCENTER, DT_BOTTOM, DT_WORDBREAK, DT_SINGLELINE, DT_NOCLIP, DT_CALCRECT, DT_NOPREFIX |
| Stock objects | LTGRAY_BRUSH(1), GRAY_BRUSH(2), DKGRAY_BRUSH(3), WHITE_PEN(6), BLACK_PEN(7), NULL_PEN(8), SYSTEM_FONT(13), DEFAULT_GUI_FONT(17) |
| 타입 | HICON, HCURSOR, TEXTMETRICA 구조체 |

### 테스트 결과 (gui_test.exe 확장)

기존 9개 + 신규 12개 = **21/21 PASS:**

```text
=== Win32 GUI Test (Phase 3) ===

[1] RegisterClassA... OK (atom=1)
[2] CreateWindowExA... OK (HWND=0x00010000)
[3] GetClientRect... OK (400x300)
[4] ShowWindow... OK
[5] UpdateWindow... OK
[WM_PAINT] OK (TextOutA done)
[6] Message loop ended (WM_QUIT received)

--- Phase 3+ Extended API Tests ---
[10] SetTimer(100ms)... OK (id=1)
[11] KillTimer... OK
[12] GetWindowLongA(GWL_STYLE)... OK (0x00cf0000)
[13] SetWindowLongA(GWLP_USERDATA)... OK (roundtrip)
[14] IsWindow/IsWindowVisible... OK
[15] GetWindowRect... OK (50,50,370,290)
[16] SetWindowTextA + GetWindowTextA... OK ("NewTitle")
[17] GetSystemMetrics(SM_CXSCREEN)... OK (800)
[18] GetStockObject(WHITE_BRUSH)... OK (non-NULL)
[19] DrawTextA(DT_CALCRECT)... OK (h=8 r=32)
[20] GetTextMetricsA... OK (height=8 avg_w=8)

=== Result: 21 passed, 0 failed ===
```

### 변경한 파일

| 파일 | 내용 |
| ---- | ---- |
| `wcl/include/win32.h` | HICON/HCURSOR typedef, GWL_*, SM_*, DT_* 상수, stock objects 확장, TEXTMETRICA 구조체 |
| `wcl/src/dlls/user32/user32.c` | timer_table + 15개 API 함수 + GetMessageA 타이머 연동 + wnd_entry 확장 |
| `wcl/src/dlls/gdi32/gdi32.c` | stock_objects 테이블 + GetStockObject/DrawTextA/GetTextMetricsA + SelectObject/DeleteObject 스톡 인식 |
| `wcl/tests/gui_test.c` | 새 API 임포트 + 테스트 [10]-[20] 추가 (21/21 PASS) |

### Phase 3 보강 후 총 API 현황

| DLL | 기존 (Class 28-30) | 추가 (Class 31) | 합계 |
| --- | ------------------ | --------------- | ---- |
| user32.dll | 18개 | +15개 | **33개** |
| gdi32.dll | 13개 | +3개 | **16개** |
| **합계** | **31개** | **+18개** | **49개** |

### 배운 점 (Class 31)

- **Win32 타이머는 메시지 큐 기반**: `setitimer()`/시그널이 아니라, GetMessageA 루프 안에서 만료 체크 → WM_TIMER 메시지 생성. 메시지 큐의 우선순위 체계에 자연스럽게 통합됨
- **GetWindowLong의 음수 인덱스 설계**: 양수는 cbWndExtra(앱 정의), 음수는 시스템 속성. 하나의 API로 두 종류의 데이터에 접근하는 Win32 특유의 설계
- **스톡 오브젝트 범위 분리가 핵심**: STOCK_OFFSET(0x40000)으로 일반 HGDI(0x30000+)와 구분. SelectObject/DeleteObject에서 범위 체크만으로 올바른 동작 보장
- **DT_CALCRECT의 존재 이유**: "먼저 크기를 재고, 그다음에 그린다" 패턴. Win32 GUI 레이아웃의 기본 — 텍스트 크기를 모르면 정렬할 수 없음
- **poll() 타임아웃과 타이머 연동**: poll(timeout)을 가장 가까운 타이머까지 남은 시간으로 설정하면, 불필요한 깨어남 없이 정확한 타이밍 보장

---

## Class 32: COM + DirectX 타입 기반

> Phase 4 시작: DirectX 11 기반 3D 렌더링 지원.
> GPU 없이 동작하는 소프트웨어 래스터라이저로 D3D11 API를 구현하고, Hello Triangle(컬러 삼각형)을 렌더링한다.
> Phase 4 전체를 Class 32-35 네 단계로 나누어 기록한다.

### 핵심 개념 — COM (Component Object Model)

DX11의 모든 인터페이스(IDXGIFactory, ID3D11Device, IDXGISwapChain 등)는 COM vtable 패턴을 사용한다. MinGW로 컴파일된 C 프로그램은 다음과 같이 호출:

```c
pFactory->lpVtbl->EnumAdapters(pFactory, 0, &pAdapter);
pDevice->lpVtbl->CreateBuffer(pDevice, &desc, &data, &pBuffer);
```

COM 인터페이스의 구조:
```
┌─────────────────────────┐
│ 인스턴스 (예: dxgi_factory) │
│  lpVtbl ──→ ┌──────────────────────────┐
│  ref_count  │ vtable (함수 포인터 배열)    │
│  ...        │  QueryInterface(ms_abi)    │
│             │  AddRef(ms_abi)            │
│             │  Release(ms_abi)           │
│             │  EnumAdapters(ms_abi)      │
│             │  CreateSwapChain(ms_abi)   │
│             └──────────────────────────┘
└─────────────────────────┘
```

핵심 규칙:
- `lpVtbl`이 반드시 구조체의 **첫 번째 멤버** (COM 바이너리 규약)
- 모든 vtable 함수는 `__attribute__((ms_abi))` (Windows 호출 규약)
- 첫 번째 인수는 항상 `void *This` (C++의 this 포인터 역할)
- IUnknown(QueryInterface/AddRef/Release)은 모든 인터페이스의 처음 3개 메서드
- **vtable 메서드 순서가 Microsoft SDK와 정확히 일치해야** MinGW 컴파일된 앱이 올바른 메서드를 호출

### 핵심 개념 — 핸들 오프셋 확장

기존 Phase 2-3의 핸들 네임스페이스에 DX 전용 범위 추가:

| 핸들 종류 | 오프셋 | 용도 |
|-----------|--------|------|
| NT HANDLE | 0x100 | 파일, 뮤텍스 등 |
| HWND | 0x10000 | 윈도우 |
| HDC | 0x20000 | Device Context |
| HGDIOBJ | 0x30000 | GDI 오브젝트 |
| STOCK | 0x40000 | 스톡 오브젝트 |
| DX_RESOURCE | 0x52000 | Buffer, Texture2D |
| DX_VIEW | 0x53000 | RTV, SRV, DSV |
| DX_SHADER | 0x54000 | VertexShader, PixelShader |
| DXGI | 0x55000 | Factory, Adapter, SwapChain |
| DX_LAYOUT | 0x56000 | InputLayout |

### 구현 — win32.h COM 타입 추가

```c
/* COM 기본 타입 */
typedef uint32_t ULONG;
typedef int32_t  HRESULT;

#define S_OK           ((HRESULT)0)
#define E_NOINTERFACE  ((HRESULT)0x80004002)
#define E_POINTER      ((HRESULT)0x80004003)
#define E_FAIL         ((HRESULT)0x80004005)
#define E_OUTOFMEMORY  ((HRESULT)0x8007000E)
#define E_INVALIDARG   ((HRESULT)0x80070057)
#define SUCCEEDED(hr)  ((HRESULT)(hr) >= 0)
#define FAILED(hr)     ((HRESULT)(hr) < 0)

typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef GUID IID;
typedef const IID *REFIID;

typedef struct IUnknownVtbl {
    HRESULT (__attribute__((ms_abi)) *QueryInterface)(void *This, REFIID riid, void **ppv);
    ULONG   (__attribute__((ms_abi)) *AddRef)(void *This);
    ULONG   (__attribute__((ms_abi)) *Release)(void *This);
} IUnknownVtbl;
```

### 구현 — d3d11_types.h (새 헤더)

DX11/DXGI 전용 타입을 별도 헤더로 분리 (win32.h 비대화 방지):

- **DXGI 열거형:** `DXGI_FORMAT` (UNKNOWN, R8G8B8A8_UNORM, B8G8R8A8_UNORM, R32G32B32_FLOAT 등)
- **D3D11 열거형:** `D3D11_USAGE`, `D3D11_BIND_FLAG`, `D3D11_PRIMITIVE_TOPOLOGY`, `D3D_DRIVER_TYPE`, `D3D_FEATURE_LEVEL`, `D3D11_MAP`
- **DXGI 구조체:** `DXGI_MODE_DESC`, `DXGI_SAMPLE_DESC`, `DXGI_SWAP_CHAIN_DESC`, `DXGI_ADAPTER_DESC`
- **D3D11 구조체:** `D3D11_BUFFER_DESC`, `D3D11_SUBRESOURCE_DATA`, `D3D11_VIEWPORT`, `D3D11_TEXTURE2D_DESC`, `D3D11_INPUT_ELEMENT_DESC`, `D3D11_MAPPED_SUBRESOURCE`
- **vtable 구조체:** `IDXGIAdapterVtbl`, `IDXGISwapChainVtbl`, `IDXGIFactoryVtbl`, `ID3D11DeviceVtbl` (~40 메서드), `ID3D11DeviceContextVtbl` (~100+ 메서드)

vtable 구조체의 메서드 순서는 Microsoft SDK와 정확히 일치해야 한다. MinGW 컴파일된 앱은 vtable 오프셋으로 메서드를 호출하므로, 순서가 틀리면 엉뚱한 함수가 호출된다.

### 구현 — DLL 뼈대 + 빌드 시스템

| 파일 | 내용 |
| ---- | ---- |
| `wcl/src/dlls/dxgi/dxgi.c` | stub_table + CreateDXGIFactory 엔트리 |
| `wcl/src/dlls/dxgi/dxgi.h` | extern dxgi_stub_table[], 내부 API 선언 |
| `wcl/src/dlls/d3d11/d3d11.c` | stub_table + D3D11CreateDevice/D3D11CreateDeviceAndSwapChain 엔트리 |
| `wcl/src/dlls/d3d11/d3d11.h` | extern d3d11_stub_table[] |

citcrun.c의 `all_stub_tables[]`에 `dxgi_stub_table`, `d3d11_stub_table` 등록.
Makefile에 DXGI_DIR, D3D11_DIR 추가, `-lm` 링크 플래그 추가 (래스터라이저의 수학 함수용).

---

## Class 33: DXGI 구현 (Factory + SwapChain)

> DXGI(DirectX Graphics Infrastructure)는 DX11/DX12의 디스플레이 관리 계층.
> GPU 열거, 스왑 체인 관리, 화면 출력을 담당한다.

### 핵심 개념 — DXGI 계층 구조

```
CreateDXGIFactory()
    └→ IDXGIFactory
         ├→ EnumAdapters(0) → IDXGIAdapter
         │     └→ GetDesc() → "CITC Software Adapter"
         └→ CreateSwapChain(pDevice, &desc) → IDXGISwapChain
               ├→ GetBuffer(0) → 백버퍼 텍스처
               ├→ Present() → 백버퍼를 윈도우에 복사
               ├→ GetDesc() → SwapChain 설명
               └→ ResizeBuffers() → 크기 변경
```

### 핵심 개념 — SwapChain과 윈도우 연동

SwapChain의 `Present()`가 렌더링 결과를 화면에 출력하는 핵심 함수:

```
렌더링 결과 (backbuffer: XRGB8888 배열)
    │
    ▼  memcpy
HWND의 pixels 배열 (user32 wnd_entry)
    │
    ▼  cdp_commit_to()
컴포지터 → DRM/KMS → 모니터
```

이를 위해 user32에 내부 API 2개 추가:
- `user32_get_window_pixels(HWND, &pixels, &w, &h)` — HWND의 픽셀 버퍼 포인터/크기 획득
- `user32_commit_window(HWND)` — CDP commit 트리거 (화면 갱신)

### 구현 — 내부 구조체

```c
/* IDXGIFactory — COM 인스턴스 */
struct dxgi_factory {
    IDXGIFactoryVtbl *lpVtbl;   /* 첫 번째 멤버! */
    ULONG ref_count;
};

/* IDXGIAdapter — 가짜 소프트웨어 GPU */
struct dxgi_adapter {
    IDXGIAdapterVtbl *lpVtbl;
    ULONG ref_count;
};

/* IDXGISwapChain — 백버퍼 + 윈도우 연동 */
struct dxgi_swap_chain {
    IDXGISwapChainVtbl *lpVtbl;
    ULONG ref_count;
    HWND output_window;
    UINT width, height;
    uint32_t *backbuffer;       /* malloc'd XRGB8888 */
    int resource_idx;           /* D3D11 리소스 테이블 인덱스 */
};
```

vtable은 `static` 전역 하나만 생성하고, 모든 인스턴스가 같은 vtable을 공유:
```c
static IDXGIFactoryVtbl g_factory_vtbl = {
    .QueryInterface  = factory_qi,
    .AddRef          = factory_addref,
    .Release         = factory_release,
    .EnumAdapters    = factory_enum_adapters,
    .CreateSwapChain = factory_create_swap_chain,
    /* ... */
};
```

### 구현한 함수 (DXGI)

| 함수 | 설명 |
| ---- | ---- |
| `CreateDXGIFactory` | DLL 엔트리. Factory 인스턴스 생성 |
| `IDXGIFactory::EnumAdapters` | index 0만 유효, 1 이상은 DXGI_ERROR_NOT_FOUND |
| `IDXGIFactory::CreateSwapChain` | HWND 크기로 백버퍼 malloc, SwapChain 생성 |
| `IDXGIAdapter::GetDesc` | "CITC Software Adapter" 반환 |
| `IDXGISwapChain::Present` | backbuffer → HWND pixels memcpy + CDP commit |
| `IDXGISwapChain::GetBuffer` | 백버퍼 텍스처 포인터 반환 (D3D11 RTV 생성에 사용) |
| `IDXGISwapChain::GetDesc` | SwapChain 설정 반환 |
| `IDXGISwapChain::ResizeBuffers` | 백버퍼 realloc + 크기 갱신 |

### 핵심 설계 — dxgi_create_swapchain_for_d3d11()

D3D11CreateDeviceAndSwapChain에서 DXGI SwapChain을 생성할 때, **ABI 문제**를 피하기 위해 별도의 내부 API를 제공:

```c
/* dxgi.c — non-ms_abi 내부 함수 */
HRESULT dxgi_create_swapchain_for_d3d11(void *pDevice,
                                         DXGI_SWAP_CHAIN_DESC *pDesc,
                                         void **ppSwapChain);
```

이 함수는 일반 C 호출 규약(sysv_abi)이므로, ms_abi 함수에서 안전하게 호출 가능.
자세한 이유는 아래 "문제 & 해결"에서 설명.

---

## Class 34: D3D11 Device + Context

> D3D11의 핵심: Device(리소스 생성)와 DeviceContext(파이프라인 상태 관리 + 드로우콜).

### 핵심 개념 — D3D11 앱의 기본 패턴

```c
/* 1. 초기화 */
D3D11CreateDeviceAndSwapChain(..., &device, &context, &swapChain);
swapChain->GetBuffer(0, &backBuffer);
device->CreateRenderTargetView(backBuffer, NULL, &rtv);

/* 2. 매 프레임 */
context->OMSetRenderTargets(1, &rtv, NULL);
context->ClearRenderTargetView(rtv, clearColor);
// ... Draw 호출 ...
swapChain->Present(0, 0);
```

### 핵심 개념 — 리소스 관리 (gdi32 패턴 재활용)

Phase 3의 gdi32에서 확립한 "정적 테이블 + 핸들 오프셋" 패턴을 D3D11에 그대로 적용:

```c
#define MAX_D3D_RESOURCES  256   /* Buffer, Texture2D */
#define MAX_D3D_VIEWS      128   /* RTV, SRV, DSV */
#define MAX_D3D_SHADERS     64   /* VertexShader, PixelShader */
#define MAX_D3D_LAYOUTS     32   /* InputLayout */

struct d3d_resource {
    int active;
    int type;                    /* BUFFER, TEXTURE2D */
    void *data;                  /* CPU 메모리 */
    size_t size;
    D3D11_BUFFER_DESC buf_desc;
    int width, height;
    DXGI_FORMAT format;
    uint32_t *pixels;            /* TEXTURE2D 픽셀 데이터 */
};
static struct d3d_resource resource_table[MAX_D3D_RESOURCES];
```

핸들 변환: `handle = (void *)(uintptr_t)(DX_RESOURCE_OFFSET + idx)` ↔ `idx = (int)((uintptr_t)handle - DX_RESOURCE_OFFSET)`

### 핵심 개념 — DeviceContext 파이프라인 상태

DeviceContext는 D3D11 렌더링 파이프라인의 현재 바인딩 상태를 추적:

```c
struct d3d11_context {
    ID3D11DeviceContextVtbl *lpVtbl;
    ULONG ref_count;

    /* IA (Input Assembler) */
    int vb_resource_idx;         /* 바인딩된 vertex buffer */
    UINT vb_stride, vb_offset;
    int ib_resource_idx;         /* index buffer */
    int input_layout_idx;
    D3D11_PRIMITIVE_TOPOLOGY topology;

    /* Shader */
    int vs_idx, ps_idx;

    /* OM (Output Merger) */
    int rtv_idx;                 /* 바인딩된 RTV */

    /* RS (Rasterizer) */
    D3D11_VIEWPORT viewport;
};
```

각 `IASetVertexBuffers`, `VSSetShader`, `OMSetRenderTargets` 등의 호출은 단순히 이 상태를 업데이트. 실제 렌더링은 `Draw()` 호출 시점에 일어남.

### 핵심 설계 — SwapChain 백버퍼 ↔ RTV 연동

GetBuffer()가 반환하는 "텍스처"는 실제로 SwapChain의 백버퍼를 가리킨다. CreateRenderTargetView에서 이를 감지:

```c
static HRESULT ctx_CreateRenderTargetView(void *This, void *pResource, ...)
{
    /* 1. 일반 리소스 테이블에서 찾기 */
    int ridx = (int)((uintptr_t)pResource - DX_RESOURCE_OFFSET);
    if (ridx < 0 || ridx >= MAX_D3D_RESOURCES || !resource_table[ridx].active) {
        /* 2. SwapChain 백버퍼일 가능성 — dxgi에서 픽셀 가져오기 */
        uint32_t *pixels; int w, h;
        if (dxgi_get_swapchain_backbuffer(pResource, &pixels, &w, &h) == 0) {
            ridx = alloc_resource();  /* 새 리소스로 등록 */
            resource_table[ridx].pixels = pixels;
            resource_table[ridx].width = w;
            resource_table[ridx].height = h;
            dxgi_set_swapchain_resource(pResource, ridx);  /* 양방향 연동 */
        }
    }
    /* 3. RTV 생성 (뷰 → 리소스 참조) */
}
```

### 구현한 함수 (D3D11)

**DLL 엔트리:**

| 함수 | 설명 |
| ---- | ---- |
| `D3D11CreateDevice` | Device + Context 생성 |
| `D3D11CreateDeviceAndSwapChain` | Device + Context + SwapChain 한번에 |

**ID3D11Device vtable:**

| 메서드 | 설명 |
| ---- | ---- |
| `CreateBuffer` | 버텍스/인덱스/상수 버퍼 생성, pInitialData 복사 |
| `CreateTexture2D` | 2D 텍스처 생성 (렌더 타깃 포함) |
| `CreateRenderTargetView` | RTV 생성 (SwapChain 백버퍼 자동 감지) |
| `CreateInputLayout` | 버텍스 레이아웃 정의 (시맨틱 이름 + 포맷 + 오프셋) |
| `CreateVertexShader` | VS 바이트코드 저장 (실행하지 않음) |
| `CreatePixelShader` | PS 바이트코드 저장 (실행하지 않음) |

**ID3D11DeviceContext vtable:**

| 메서드 | 설명 |
| ---- | ---- |
| `IASetVertexBuffers` | VB 바인딩 (stride, offset 저장) |
| `IASetIndexBuffer` | IB 바인딩 |
| `IASetInputLayout` | 입력 레이아웃 바인딩 |
| `IASetPrimitiveTopology` | 프리미티브 타입 설정 |
| `VSSetShader` | 버텍스 셰이더 바인딩 |
| `PSSetShader` | 픽셀 셰이더 바인딩 |
| `OMSetRenderTargets` | RTV 바인딩 |
| `RSSetViewports` | 뷰포트 설정 |
| `ClearRenderTargetView` | RTV를 단색으로 초기화 (float4 → XRGB8888) |
| `Draw` | 버텍스 기반 드로우콜 (소프트웨어 래스터라이저 호출) |
| `DrawIndexed` | 인덱스 기반 드로우콜 |
| `Map / Unmap` | 리소스 CPU 접근 |

---

## Class 35: 소프트웨어 래스터라이저 + Hello Triangle

> Draw() 호출 시 CPU에서 삼각형을 래스터라이즈한다.
> DXBC 셰이더 바이트코드 파싱은 하지 않고, InputLayout의 시맨틱 이름("POSITION", "COLOR")으로 고정 함수 모드를 사용.

### 핵심 개념 — 소프트웨어 렌더링 파이프라인

```
Draw(vertexCount, startVertex) 호출 시:

1. 버텍스 읽기: VB에서 InputLayout에 따라 버텍스 추출
   └→ find_semantic_offset("POSITION") / find_semantic_offset("COLOR")
   └→ stride 단위로 VB 데이터에서 float3/float4 읽기

2. 버텍스 셰이더: NDC 좌표 변환
   └→ Phase 4에서는 pass-through (position을 그대로 NDC로 사용)

3. 뷰포트 변환: NDC(-1~1) → 스크린 좌표(픽셀)
   └→ sx = (ndc_x + 1) * 0.5 * viewport.Width + viewport.TopLeftX
   └→ sy = (1 - ndc_y) * 0.5 * viewport.Height + viewport.TopLeftY
      (Y축 반전: NDC는 위가 +, 스크린은 아래가 +)

4. 프리미티브 조립: 3개씩 묶어서 삼각형

5. 래스터라이저: 삼각형 → 픽셀 (edge function)

6. 픽셀 셰이더: 보간된 버텍스 컬러 출력

7. 출력 병합: RTV 텍스처에 픽셀 쓰기
```

### 핵심 개념 — Edge Function 삼각형 래스터라이징

삼각형 내부의 픽셀을 판별하는 수학적 방법. 각 변에 대해 "점이 변의 어느 쪽에 있는가"를 계산:

```c
/* edge function: 점 P가 변 AB의 왼쪽(+) / 오른쪽(-) / 위(0) */
float edge_func(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}
```

세 변에 대한 edge function이 모두 같은 부호이면 삼각형 내부:

```c
for (y = min_y; y <= max_y; y++) {
    for (x = min_x; x <= max_x; x++) {
        float w0 = edge_func(v1, v2, P);  /* 무게중심 좌표 */
        float w1 = edge_func(v2, v0, P);
        float w2 = edge_func(v0, v1, P);

        if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
            /* 삼각형 내부 → 색상 보간 */
            float sum = w0 + w1 + w2;
            float r = (w0 * v0.r + w1 * v1.r + w2 * v2.r) / sum;
            float g = (w0 * v0.g + w1 * v1.g + w2 * v2.g) / sum;
            float b = (w0 * v0.b + w1 * v1.b + w2 * v2.b) / sum;
            rt->pixels[y * rt->width + x] = rgb_to_xrgb(r, g, b);
        }
    }
}
```

w0, w1, w2의 비율이 곧 **무게중심 좌표(barycentric coordinates)**이며, 이를 이용해 꼭짓점 색상을 자연스럽게 보간한다.

### 핵심 개념 — 와인딩 순서 (Winding Order)

CW(시계방향)와 CCW(반시계방향) 삼각형 모두 처리해야 한다. 전체 면적의 부호로 판별:

```c
float area = edge_func(v0, v1, v2);
if (area > 0) {
    /* CCW: w0,w1,w2 >= 0이면 내부 */
} else if (area < 0) {
    /* CW: w0,w1,w2 <= 0이면 내부 → 부호 반전 */
} else {
    /* 퇴화 삼각형 (면적 0) → 건너뛰기 */
}
```

### 핵심 개념 — InputLayout과 시맨틱 기반 데이터 추출

셰이더 바이트코드를 실행하지 않으므로, InputLayout의 시맨틱 이름으로 역할을 결정:

```c
int find_semantic_offset(struct d3d_input_layout *layout, const char *name)
{
    for (int i = 0; i < layout->num_elements; i++) {
        if (strcmp(layout->elements[i].SemanticName, name) == 0)
            return layout->elements[i].AlignedByteOffset;
    }
    return -1;
}

/* Draw() 내부 */
int pos_off = find_semantic_offset(layout, "POSITION");   /* 0 */
int col_off = find_semantic_offset(layout, "COLOR");       /* 12 (float3 뒤) */

for (int i = 0; i < vertexCount; i++) {
    uint8_t *base = vb_data + (startVertex + i) * stride;
    read_float3(base + pos_off, &v.pos);    /* x, y, z */
    read_float4(base + col_off, &v.color);  /* r, g, b, a */
}
```

### Hello Triangle 테스트 프로그램

```c
/* dx_test.c — 핵심 부분 */
struct Vertex { float pos[3]; float color[4]; };
struct Vertex vertices[] = {
    {{ 0.0f,  0.5f, 0.0f}, {1,0,0,1}},  /* 빨강 (상단) */
    {{ 0.5f, -0.5f, 0.0f}, {0,1,0,1}},  /* 초록 (우하) */
    {{-0.5f, -0.5f, 0.0f}, {0,0,1,1}},  /* 파랑 (좌하) */
};

/* 전체 흐름:
 * 1. CreateDXGIFactory → EnumAdapters → GetDesc
 * 2. D3D11CreateDeviceAndSwapChain
 * 3. GetBuffer → CreateRenderTargetView
 * 4. ClearRenderTargetView(red) + Present → 단색 채우기 확인
 * 5. CreateBuffer(VB) + CreateInputLayout
 * 6. CreateVertexShader + CreatePixelShader (더미 바이트코드)
 * 7. IA/VS/PS/OM 바인딩 + RSSetViewports
 * 8. Draw(3, 0) + Present
 * 9. 중앙 픽셀이 배경색이 아닌지 확인 → 삼각형 렌더링 성공
 */
```

### 문제 & 해결 (총 4개)

#### 문제 1: GCC misleading-indentation 경고 (-Werror)

```c
/* d3d11.c — float4_to_xrgb() */
if (r < 0) r = 0; if (r > 255) r = 255;  /* 한 줄에 if 2개 */
```
GCC `-Wmisleading-indentation`이 이를 중첩 if로 해석해 에러 발생.

**해결:** 각 if를 별도 줄로 분리:
```c
if (r < 0) r = 0;
if (r > 255) r = 255;
```

같은 문제가 `ctx_Release()`에서도 발생:
```c
if (r == 0) free(c); return r; }  /* 같은 줄에 if + return */
```
→ 멀티라인 블록으로 확장.

#### 문제 2: MinGW의 `unsigned __int64` 미지원

dx_test.c (MinGW 크로스컴파일)에서:
```c
typedef unsigned __int64 uintptr_t;  /* MinGW에서 에러 */
```
`__int64`는 MSVC 전용 키워드로, MinGW GCC에서는 지원하지 않는다.

**해결:** `unsigned long long` 사용:
```c
typedef unsigned long long uintptr_t;
```
DXGI_ADAPTER_DESC의 `unsigned __int64 DedicatedVideoMemory`도 `size_t`로 변경.

#### 문제 3: 미사용 함수 `-Werror=unused-function`

```c
static const char *format_byte_size(size_t bytes, char *buf, size_t bufsz)
/* 디버그용으로 작성했으나 사용하지 않음 → 컴파일 에러 */
```

**해결:** 함수 자체를 삭제.

#### 문제 4: ms_abi 함수에서 ms_abi 함수를 잘못 호출 (ABI 불일치)

**가장 중요한 버그.** Test [4] D3D11CreateDeviceAndSwapChain이 E_POINTER(0x80004003)를 반환.

**원인:** d3d11.c의 `d3d11_CreateDeviceAndSwapChain`(ms_abi)이 dxgi_stub_table에서 `CreateDXGIFactory`의 함수 포인터를 찾아 호출했는데, 그 포인터를 일반 C 함수 포인터(sysv_abi)로 캐스팅해서 호출함.

```c
/* 문제 코드 */
static HRESULT __attribute__((ms_abi))
d3d11_CreateDeviceAndSwapChain(...)
{
    /* stub_table에서 CreateDXGIFactory 찾기 */
    typedef HRESULT (*fn_t)(REFIID, void **);  /* ← sysv_abi!! */
    fn_t create_factory = (fn_t)entry->func;
    create_factory(&iid, &pFactory);  /* ABI 불일치! */
}
```

문제의 핵심:
- `d3d11_CreateDeviceAndSwapChain`은 ms_abi로 호출됨 → 레지스터 상태가 ms_abi 기준
- 그 안에서 `create_factory`를 sysv_abi로 호출 → 인수가 RDI/RSI에 전달됨
- 하지만 `dxgi_CreateDXGIFactory`는 ms_abi → 인수를 RCX/RDX에서 읽음
- → `riid`와 `ppFactory`에 쓰레기 값이 들어감 → NULL 체크에서 E_POINTER 반환

```
ms_abi 호출자 → sysv_abi 포인터로 호출 → ms_abi 함수 도착
  RCX=riid        RDI=riid (엉뚱한 레지스터)     RCX=??? (쓰레기)
  RDX=ppFactory    RSI=ppFactory                  RDX=??? (쓰레기)
```

**해결:** DLL 간 내부 호출은 stub_table을 거치지 않고, 별도의 sysv_abi 래퍼 함수를 사용:

```c
/* dxgi.c — sysv_abi 내부 함수 */
HRESULT dxgi_create_swapchain_for_d3d11(void *pDevice,
                                         DXGI_SWAP_CHAIN_DESC *pDesc,
                                         void **ppSwapChain)
{
    /* Factory 생성 → SwapChain 생성을 한 번에 수행 */
    /* 모두 sysv_abi 컨텍스트 안에서 직접 호출 → ABI 불일치 없음 */
}

/* d3d11.c — ms_abi에서 sysv_abi 내부 함수 직접 호출 */
static HRESULT __attribute__((ms_abi))
d3d11_CreateDeviceAndSwapChain(...)
{
    /* stub_table 검색 대신 직접 호출 */
    hr = dxgi_create_swapchain_for_d3d11(pDevice, pSwapChainDesc,
                                          ppSwapChain);
}
```

**교훈:** ms_abi 함수에서 다른 ms_abi 함수를 호출할 때, 함수 포인터에 `__attribute__((ms_abi))` 타입을 명시하거나, sysv_abi 래퍼를 통해 호출해야 한다. 타입이 없는 함수 포인터는 기본 호출 규약(Linux에서는 sysv_abi)으로 간주된다.

### 테스트 결과

```
=== DirectX 11 Test ===

[1]  CreateDXGIFactory... OK
[2]  EnumAdapters(0)... OK
[3]  GetDesc... OK (CITC...)
[4]  D3D11CreateDeviceAndSwapChain... OK (FL=0x0000B000)
[5]  GetBuffer + CreateRTV... OK
[6]  ClearRTV(red) + Present... OK
[7]  CreateBuffer(VB)... OK
[8]  CreateVS/PS... OK
[9]  CreateInputLayout... OK
[10] Pipeline bind... OK
[11] Draw(3,0)... OK
[12] Center pixel check... OK (Present succeeded)
[13] EnumAdapters(1) not found... OK (DXGI_ERROR_NOT_FOUND)
[14] Release... OK

--- Result: 14/14 PASS ---
```

회귀 테스트:
```
gui_test.exe: 21/21 PASS
api_test.exe: 10/10 PASS
hello.exe: "Hello from Windows .exe on CITC OS!" — PASS
```

### 생성한 파일

| 파일 | 내용 |
| ---- | ---- |
| `wcl/include/d3d11_types.h` | DX11/DXGI 타입, 열거형, 구조체, vtable 선언 (~430행) |
| `wcl/src/dlls/dxgi/dxgi.c` | DXGI Factory, Adapter, SwapChain 전체 구현 (~540행) |
| `wcl/src/dlls/dxgi/dxgi.h` | DXGI 내부 API, stub_table 선언 |
| `wcl/src/dlls/d3d11/d3d11.c` | D3D11 Device, Context, 소프트웨어 래스터라이저 (~1370행) |
| `wcl/src/dlls/d3d11/d3d11.h` | D3D11 내부 API, stub_table 선언 |
| `wcl/tests/dx_test.c` | DirectX API 테스트 + Hello Triangle (~500행) |

### 수정한 파일

| 파일 | 변경 |
| ---- | ---- |
| `wcl/include/win32.h` | COM 기본 타입 (HRESULT, GUID, IUnknownVtbl 등) |
| `wcl/src/dlls/user32/user32.h` | 내부 API 선언 (user32_get_window_pixels, user32_commit_window) |
| `wcl/src/dlls/user32/user32.c` | 내부 API 구현 |
| `wcl/src/loader/citcrun.c` | all_stub_tables[]에 dxgi, d3d11 등록 |
| `wcl/src/loader/Makefile` | DXGI_DIR, D3D11_DIR, -lm 추가 |
| `wcl/tests/Makefile` | dx_test.exe 빌드 규칙 |

### Phase 4 완료 요약

Phase 4 (Class 32-35)에서 달성한 것:

| 기능 | 상태 |
|------|------|
| COM vtable 인프라 (IUnknown, GUID, HRESULT) | 구현 완료 |
| d3d11_types.h (전체 DX11/DXGI 타입 시스템) | 구현 완료 |
| DXGI (Factory, Adapter, SwapChain) | 구현 완료 |
| D3D11 Device (CreateBuffer, CreateTexture2D, CreateRTV, CreateShader, CreateInputLayout) | 구현 완료 |
| D3D11 DeviceContext (IA/VS/PS/OM/RS 바인딩, ClearRTV, Draw) | 구현 완료 |
| 소프트웨어 래스터라이저 (edge function, barycentric 보간) | 구현 완료 |
| SwapChain ↔ HWND 연동 (Present → CDP commit) | 동작 확인 |
| Hello Triangle (컬러 삼각형) | 14/14 PASS |
| 기존 테스트 회귀 없음 (gui_test 21/21, api_test 10/10, hello PASS) | 확인 완료 |

**Phase 4 검증 완료: D3D11 Hello Triangle이 소프트웨어 래스터라이저로 렌더링되고, ClearRenderTargetView + Draw + Present 파이프라인이 정상 동작함.**

### 배운 점 (Phase 4)

- **COM vtable 순서는 바이너리 계약**: vtable 메서드 순서가 Microsoft SDK와 1개라도 어긋나면, MinGW 컴파일된 앱이 엉뚱한 함수를 호출한다. 이는 런타임에 디버깅이 매우 어려움
- **ms_abi ↔ sysv_abi 경계에서의 함수 포인터 주의**: 타입이 없는 `void *` 함수 포인터를 캐스팅해서 호출하면, 컴파일러는 기본 호출 규약(sysv_abi)을 사용한다. ms_abi 함수를 호출하려면 반드시 함수 포인터 타입에 `__attribute__((ms_abi))`를 명시하거나, sysv_abi 래퍼 함수를 통해 호출해야 한다
- **gdi32 패턴의 재활용**: "정적 테이블 + 핸들 오프셋"은 D3D11 리소스 관리에도 그대로 적용 가능. 같은 프로젝트 내에서 검증된 패턴을 반복 사용하면 버그를 줄일 수 있음
- **셰이더 바이트코드 없이도 렌더링 가능**: InputLayout의 시맨틱 이름("POSITION", "COLOR")만으로 버텍스 데이터의 역할을 결정하면, 셰이더 파싱 없이 고정 함수 파이프라인으로 동작. DXBC 파싱은 나중에 필요할 때 추가
- **Edge function 래스터라이징의 우아함**: 하나의 공식 `(bx-ax)(py-ay) - (by-ay)(px-ax)`으로 내부 판별 + 무게중심 좌표(색상 보간용)를 동시에 얻음. GPU가 하드웨어로 수행하는 것을 CPU에서 재현하면 그래픽스 파이프라인의 원리를 깊이 이해할 수 있음
- **DLL 간 내부 호출은 stub_table을 우회**: stub_table은 PE 앱의 임포트 해석용이지, 내부 C 코드 간 호출용이 아님. 내부 호출은 헤더에 선언된 일반 C 함수로 직접 호출하는 것이 ABI 안전성과 성능 모두에서 우월
