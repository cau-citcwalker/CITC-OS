# CITC OS

Linux 커널 기반으로 **Linux 앱과 Windows 앱을 모두 네이티브로 실행**할 수 있는 PC용 운영체제.

Windows 호환은 자체 호환 레이어(CITC-WCL)를 통해 달성한다.

## 아키텍처

```
+============================================================+
|                    CITC OS USER SPACE                       |
|                                                             |
|  +------------------+  +------------------+  +-----------+  |
|  | CITC Desktop Env |  | citcpkg          |  | citcinit  |  |
|  | (Wayland 컴포지터)|  | (패키지 매니저)   |  | (init)    |  |
|  +------------------+  +------------------+  +-----------+  |
|                                                             |
|  +------------------------------------------------------+   |
|  | Linux 앱 (ELF)  |  Windows 호환 레이어 (CITC-WCL)    |   |
|  |                  |  +------+ +------+ +--------+      |   |
|  |                  |  | PE   | | NT   | | Win32  |      |   |
|  |                  |  |Loader| | 에뮬 | | API    |      |   |
|  |                  |  +------+ +------+ +--------+      |   |
|  |                  |  +-------+ +--------+              |   |
|  |                  |  | DX11  | |Registry|              |   |
|  |                  |  | S/W   | |Service |              |   |
|  |                  |  +-------+ +--------+              |   |
|  +------------------------------------------------------+   |
|                                                             |
|  +------------------------------------------------------+   |
|  | 시스템 서비스: Display Server | Audio | IPC | Security |   |
|  +------------------------------------------------------+   |
+============================================================+
|              LINUX KERNEL (커스텀 설정)                      |
|  드라이버: GPU, USB, NVMe, 네트워크, 오디오 등               |
+============================================================+
```

## 주요 컴포넌트

| 컴포넌트 | 설명 | 언어 |
|----------|------|------|
| **citcinit** | PID 1 init 시스템 (서비스 관리, 소켓 활성화) | C |
| **citcpkg** | 패키지 매니저 (로컬 + 원격 저장소) | C |
| **citcsh** | 커스텀 쉘 (파이프, 리다이렉션, 빌트인) | C |
| **compositor** | Wayland 기반 컴포지터 (DRM/KMS) | C |
| **citcterm** | 터미널 에뮬레이터 (PTY + ANSI) | C |
| **citcshell** | 데스크탑 셸 (태스크바 + 앱 런처) | C |
| **CITC-WCL** | Windows 호환 레이어 (PE 로더 + Win32 API) | C |

## CITC-WCL (Windows Compatibility Layer)

MinGW로 크로스컴파일된 Windows .exe를 Linux 위에서 직접 실행한다.

### 구현된 DLL

| DLL | API 수 | 주요 기능 |
|-----|--------|-----------|
| **kernel32.dll** | 16개 | 파일 I/O, 메모리, 프로세스, 환경변수 |
| **ntdll.dll** | 8개 | NT 시스콜 에뮬, Object Manager |
| **user32.dll** | 33개 | 윈도우 관리, 메시지 루프, 입력, 타이머 |
| **gdi32.dll** | 16개 | 2D 그래픽, 텍스트, 브러시, 스톡 오브젝트 |
| **dxgi.dll** | 8개 | DXGI Factory, Adapter, SwapChain |
| **d3d11.dll** | 20개+ | D3D11 Device, Context, 소프트웨어 래스터라이저 |

### PE 로더 흐름

```
.exe → DOS 헤더("MZ") → PE 헤더 파싱 → 섹션 mmap
→ 리로케이션 적용 → 임포트 해석 (kernel32.dll → 내장 구현)
→ TLS 초기화 → 엔트리포인트 실행
```

### 소프트웨어 래스터라이저

GPU 없이 CPU에서 D3D11 렌더링 파이프라인을 실행:

- Edge function 삼각형 래스터라이징
- Barycentric 좌표 기반 버텍스 컬러 보간
- InputLayout 시맨틱("POSITION", "COLOR") 기반 고정 함수 파이프라인

## 빌드

### 필요 도구

- GCC 13+ (호스트 컴파일러)
- x86_64-w64-mingw32-gcc (Windows 테스트 프로그램용)
- QEMU (테스트 환경)

### 빌드 & 실행

```bash
# WCL 로더 빌드
make -C wcl/src/loader clean && make -C wcl/src/loader

# 테스트 프로그램 빌드 (MinGW 크로스컴파일)
make -C wcl/tests clean && make -C wcl/tests

# Windows .exe 실행
./wcl/src/loader/build/citcrun ./wcl/tests/build/hello.exe
# → "Hello from Windows .exe on CITC OS!"

# QEMU 부팅 (전체 시스템 테스트)
bash tools/mkrootfs/build-initramfs.sh
bash tools/run-qemu.sh --gui
```

## 테스트 현황

```
hello.exe:    "Hello from Windows .exe on CITC OS!" — PASS
api_test.exe: 10/10 PASS (메모리, 환경변수, 프로세스)
gui_test.exe: 21/21 PASS (윈도우, 메시지, GDI, 타이머)
dx_test.exe:  14/14 PASS (DXGI, D3D11, 소프트웨어 래스터라이저)
```

## 프로젝트 구조

```
citc_os/
├── kernel/config/          # Linux 커널 커스텀 설정
├── system/
│   ├── citcinit/           # init 시스템
│   ├── citcpkg/            # 패키지 매니저
│   ├── citcsh/             # 커스텀 쉘
│   └── citc-ipc/           # IPC 메시지 버스
├── display/
│   ├── compositor/         # Wayland 컴포지터
│   ├── protocol/           # CDP (CITC Display Protocol)
│   ├── terminal/           # 터미널 에뮬레이터
│   └── shell/              # 데스크탑 셸
├── wcl/                    # Windows Compatibility Layer
│   ├── include/            # PE, Win32, D3D11 타입 헤더
│   ├── src/
│   │   ├── loader/         # PE 로더 + citcrun
│   │   ├── ntemu/          # NT 에뮬레이션
│   │   └── dlls/           # Win32 DLL 구현
│   │       ├── kernel32/
│   │       ├── user32/
│   │       ├── gdi32/
│   │       ├── dxgi/
│   │       └── d3d11/
│   └── tests/              # MinGW 크로스컴파일 테스트
└── tools/                  # 빌드 스크립트, QEMU 도구
```

## 로드맵

- [x] **Phase 0**: 커널 빌드, init 시스템, QEMU 부팅
- [x] **Phase 1**: 컴포지터, 패키지 매니저, 데스크탑 셸
- [x] **Phase 2**: PE 로더, NT 에뮬, kernel32/ntdll
- [x] **Phase 3**: user32, gdi32, Win32 GUI
- [x] **Phase 4**: DXGI, D3D11, 소프트웨어 래스터라이저
- [ ] **Phase 5**: Vulkan 백엔드, 셰이더 컴파일러, 오디오

## 라이선스

[MIT License](LICENSE)
