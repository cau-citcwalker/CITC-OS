/*
 * pe.h — PE (Portable Executable) 포맷 정의
 * ============================================
 *
 * PE는 Windows의 실행 파일 형식입니다 (Linux의 ELF에 대응).
 *
 * PE vs ELF 비교:
 *   PE:  DOS Header → PE Header → Section Headers → Sections
 *   ELF: ELF Header → Program Headers → Section Headers → Sections
 *
 *   둘 다 같은 목표: "이 바이너리를 메모리에 어떻게 배치하고 실행할까?"
 *
 * 왜 "MZ"와 "PE"가 같이 있는가?
 *   MZ = Mark Zbikowski (DOS 개발자 이름, 1983년)
 *   PE = Portable Executable (Windows NT, 1993년)
 *   하나의 .exe가 DOS에서도, Windows에서도 실행 가능하도록 설계됨.
 *   DOS에서 실행하면 → "This program cannot be run in DOS mode" 출력.
 *   Windows에서 실행하면 → e_lfanew로 PE 헤더에 점프하여 정상 실행.
 *
 * 참고: https://learn.microsoft.com/en-us/windows/win32/debug/pe-format
 */

#ifndef CITC_PE_H
#define CITC_PE_H

#include <stdint.h>

/* ============================================================
 * 매직 넘버 & 상수
 * ============================================================ */

#define IMAGE_DOS_SIGNATURE	0x5A4D		/* "MZ" — DOS 실행 파일 시그니처 */
#define IMAGE_NT_SIGNATURE	0x00004550	/* "PE\0\0" — PE 시그니처 */

#define IMAGE_FILE_MACHINE_AMD64	0x8664	/* x86_64 */
#define IMAGE_FILE_MACHINE_I386		0x014C	/* x86 (32비트) */

#define IMAGE_NT_OPTIONAL_HDR64_MAGIC	0x020B	/* PE32+ (64비트) */
#define IMAGE_NT_OPTIONAL_HDR32_MAGIC	0x010B	/* PE32 (32비트) */

/* ============================================================
 * 1. DOS Header (64바이트)
 * ============================================================
 *
 * 모든 PE 파일의 첫 64바이트. 1983년 DOS 시절의 유산.
 * 현대 Windows에서 의미 있는 필드는 단 2개:
 *   - e_magic: "MZ" 시그니처 (파일이 실행 파일임을 확인)
 *   - e_lfanew: PE 헤더의 파일 내 오프셋 (나머지를 건너뛸 수 있게 함)
 *
 * ELF 대응: ELF 헤더의 e_ident[0..3] = "\x7FELF"
 */
struct __attribute__((packed)) IMAGE_DOS_HEADER {
	uint16_t e_magic;	/* 0x00: "MZ" (0x5A4D) */
	uint16_t e_cblp;	/* 0x02: 마지막 페이지 바이트 수 (무시) */
	uint16_t e_cp;		/* 0x04: 페이지 수 (무시) */
	uint16_t e_crlc;	/* 0x06: 리로케이션 수 (무시) */
	uint16_t e_cparhdr;	/* 0x08: 헤더 크기 (무시) */
	uint16_t e_minalloc;	/* 0x0A: (무시) */
	uint16_t e_maxalloc;	/* 0x0C: (무시) */
	uint16_t e_ss;		/* 0x0E: (무시) */
	uint16_t e_sp;		/* 0x10: (무시) */
	uint16_t e_csum;	/* 0x12: (무시) */
	uint16_t e_ip;		/* 0x14: (무시) */
	uint16_t e_cs;		/* 0x16: (무시) */
	uint16_t e_lfarlc;	/* 0x18: (무시) */
	uint16_t e_ovno;	/* 0x1A: (무시) */
	uint16_t e_res[4];	/* 0x1C: (무시) */
	uint16_t e_oemid;	/* 0x24: (무시) */
	uint16_t e_oeminfo;	/* 0x26: (무시) */
	uint16_t e_res2[10];	/* 0x28: (무시) */
	uint32_t e_lfanew;	/* 0x3C: ★ PE 헤더 오프셋 — 이것만 중요! */
};

/* ============================================================
 * 2. COFF File Header (20바이트)
 * ============================================================
 *
 * PE 시그니처("PE\0\0") 바로 뒤에 위치.
 * CPU 아키텍처, 섹션 수 등 기본 정보.
 *
 * ELF 대응: e_machine, e_shnum
 */
struct __attribute__((packed)) IMAGE_FILE_HEADER {
	uint16_t Machine;		/* CPU 타입 (0x8664 = AMD64) */
	uint16_t NumberOfSections;	/* 섹션 수 (.text, .data 등) */
	uint32_t TimeDateStamp;		/* 빌드 시각 (Unix timestamp) */
	uint32_t PointerToSymbolTable;	/* 심볼 테이블 오프셋 (보통 0) */
	uint32_t NumberOfSymbols;	/* 심볼 수 (보통 0) */
	uint16_t SizeOfOptionalHeader;	/* Optional Header 크기 */
	uint16_t Characteristics;	/* 플래그 (실행 가능, DLL 등) */
};

/* File Header Characteristics 플래그 */
#define IMAGE_FILE_EXECUTABLE_IMAGE	0x0002	/* 실행 가능 */
#define IMAGE_FILE_LARGE_ADDRESS_AWARE	0x0020	/* 2GB+ 주소 사용 가능 */
#define IMAGE_FILE_DLL			0x2000	/* DLL 파일 */

/* ============================================================
 * 3. Data Directory (8바이트)
 * ============================================================
 *
 * Optional Header에 포함된 배열. 각 엔트리가
 * 특정 데이터(임포트, 리로케이션 등)의 위치와 크기를 알려줌.
 */
struct __attribute__((packed)) IMAGE_DATA_DIRECTORY {
	uint32_t VirtualAddress;	/* 데이터의 RVA (상대 가상 주소) */
	uint32_t Size;			/* 데이터 크기 (바이트) */
};

/* Data Directory 인덱스 상수 */
#define IMAGE_DIRECTORY_ENTRY_EXPORT	0	/* Export Table */
#define IMAGE_DIRECTORY_ENTRY_IMPORT	1	/* ★ Import Table */
#define IMAGE_DIRECTORY_ENTRY_RESOURCE	2	/* Resource Table */
#define IMAGE_DIRECTORY_ENTRY_EXCEPTION	3	/* Exception Table */
#define IMAGE_DIRECTORY_ENTRY_BASERELOC	5	/* ★ Base Relocation Table */
#define IMAGE_DIRECTORY_ENTRY_DEBUG	6	/* Debug Directory */
#define IMAGE_DIRECTORY_ENTRY_TLS	9	/* TLS Directory */
#define IMAGE_DIRECTORY_ENTRY_IAT	12	/* Import Address Table */
#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES	16

/* ============================================================
 * 4. Optional Header — PE32+ (240바이트)
 * ============================================================
 *
 * "Optional"이라는 이름이지만, 실행 파일에서는 필수!
 * (오브젝트 파일(.obj)에서만 선택적)
 *
 * PE 파일의 가장 중요한 정보:
 *   - ImageBase: 메모리에 로드될 기본 주소
 *   - AddressOfEntryPoint: 실행 시작 위치 (RVA)
 *   - SizeOfImage: 로드 후 전체 메모리 크기
 *   - DataDirectory: 임포트, 리로케이션 등의 위치
 *
 * ELF 대응: Program Header (LOAD 세그먼트), e_entry
 *
 * PE32 (32비트)와 PE32+ (64비트)의 차이:
 *   - Magic: 0x10B (PE32) vs 0x20B (PE32+)
 *   - ImageBase: 4바이트 vs 8바이트
 *   - 일부 필드 크기 차이
 */
struct __attribute__((packed)) IMAGE_OPTIONAL_HEADER64 {
	/* Standard fields */
	uint16_t Magic;			/* 0x20B = PE32+ */
	uint8_t  MajorLinkerVersion;
	uint8_t  MinorLinkerVersion;
	uint32_t SizeOfCode;
	uint32_t SizeOfInitializedData;
	uint32_t SizeOfUninitializedData;
	uint32_t AddressOfEntryPoint;	/* ★ 엔트리포인트 RVA */
	uint32_t BaseOfCode;

	/* PE32+ specific fields (64비트) */
	uint64_t ImageBase;		/* ★ 기본 로드 주소 (보통 0x140000000) */
	uint32_t SectionAlignment;	/* 메모리 정렬 (보통 4096 = 페이지 크기) */
	uint32_t FileAlignment;		/* 파일 정렬 (보통 512) */
	uint16_t MajorOperatingSystemVersion;
	uint16_t MinorOperatingSystemVersion;
	uint16_t MajorImageVersion;
	uint16_t MinorImageVersion;
	uint16_t MajorSubsystemVersion;
	uint16_t MinorSubsystemVersion;
	uint32_t Win32VersionValue;
	uint32_t SizeOfImage;		/* ★ 로드 후 전체 크기 */
	uint32_t SizeOfHeaders;		/* 모든 헤더 + 섹션 헤더의 크기 */
	uint32_t CheckSum;
	uint16_t Subsystem;		/* GUI(2) vs Console(3) */
	uint16_t DllCharacteristics;
	uint64_t SizeOfStackReserve;
	uint64_t SizeOfStackCommit;
	uint64_t SizeOfHeapReserve;
	uint64_t SizeOfHeapCommit;
	uint32_t LoaderFlags;
	uint32_t NumberOfRvaAndSizes;	/* DataDirectory 배열 크기 (보통 16) */

	/* Data Directory 배열 */
	struct IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
};

/* Subsystem 상수 */
#define IMAGE_SUBSYSTEM_WINDOWS_GUI	2	/* GUI 앱 */
#define IMAGE_SUBSYSTEM_WINDOWS_CUI	3	/* 콘솔 앱 */

/* ============================================================
 * 5. Section Header (40바이트)
 * ============================================================
 *
 * 각 섹션의 메모리 배치와 파일 위치를 정의.
 *
 * 주요 섹션들:
 *   .text   — 실행 코드 (기계어)
 *   .rdata  — 읽기 전용 데이터 (문자열 상수, 임포트 테이블)
 *   .data   — 읽기/쓰기 데이터 (전역 변수)
 *   .reloc  — 베이스 리로케이션 테이블
 *   .idata  — 임포트 디렉토리 (rdata에 합쳐지기도 함)
 *
 * ELF 대응: Section Header (.text, .rodata, .data 등)
 */
#define IMAGE_SIZEOF_SHORT_NAME	8

struct __attribute__((packed)) IMAGE_SECTION_HEADER {
	char     Name[IMAGE_SIZEOF_SHORT_NAME]; /* 섹션 이름 (8바이트, null 패딩) */
	uint32_t VirtualSize;		/* 메모리에서의 크기 */
	uint32_t VirtualAddress;	/* ★ RVA: 메모리 배치 위치 */
	uint32_t SizeOfRawData;		/* 파일에서의 크기 */
	uint32_t PointerToRawData;	/* ★ 파일 내 오프셋 */
	uint32_t PointerToRelocations;	/* (오브젝트 파일용, 무시) */
	uint32_t PointerToLinenumbers;	/* (무시) */
	uint16_t NumberOfRelocations;	/* (무시) */
	uint16_t NumberOfLinenumbers;	/* (무시) */
	uint32_t Characteristics;	/* ★ 플래그: 읽기/쓰기/실행 */
};

/* Section Characteristics 플래그 */
#define IMAGE_SCN_CNT_CODE		0x00000020	/* 코드 포함 */
#define IMAGE_SCN_CNT_INITIALIZED_DATA	0x00000040	/* 초기화된 데이터 */
#define IMAGE_SCN_MEM_EXECUTE		0x20000000	/* 실행 가능 */
#define IMAGE_SCN_MEM_READ		0x40000000	/* 읽기 가능 */
#define IMAGE_SCN_MEM_WRITE		0x80000000	/* 쓰기 가능 */

/* ============================================================
 * 6. Import Directory (20바이트)
 * ============================================================
 *
 * 하나의 DLL에 대한 임포트 정보.
 * Import Table은 이 구조체의 배열 (마지막은 all-zero).
 *
 * 임포트 해석 흐름:
 *   1. Import Directory에서 DLL 이름 읽기 ("kernel32.dll")
 *   2. ILT (Import Lookup Table)에서 함수 이름 목록 읽기
 *   3. 각 함수의 실제 주소를 찾아 IAT에 쓰기
 *   4. .exe 코드는 IAT를 통해 함수를 호출!
 *
 * ELF 대응: .dynamic 섹션의 DT_NEEDED + GOT/PLT
 */
struct __attribute__((packed)) IMAGE_IMPORT_DESCRIPTOR {
	uint32_t OriginalFirstThunk;	/* ILT (Import Lookup Table) RVA */
	uint32_t TimeDateStamp;		/* 바인딩 시각 (보통 0) */
	uint32_t ForwarderChain;	/* 포워딩 체인 (보통 -1 또는 0) */
	uint32_t Name;			/* ★ DLL 이름 문자열 RVA */
	uint32_t FirstThunk;		/* ★ IAT (Import Address Table) RVA */
};

/*
 * Import Lookup Table (ILT) 엔트리 — 64비트
 *
 * bit 63 = 1: 서수(ordinal)로 임포트 (하위 16비트 = 서수 번호)
 * bit 63 = 0: 이름으로 임포트 (하위 31비트 = IMAGE_IMPORT_BY_NAME RVA)
 */
#define IMAGE_ORDINAL_FLAG64	0x8000000000000000ULL

/* ============================================================
 * 7. Import By Name (가변 길이)
 * ============================================================ */
struct __attribute__((packed)) IMAGE_IMPORT_BY_NAME {
	uint16_t Hint;		/* Export Table 인덱스 힌트 (최적화용) */
	char     Name[1];	/* 함수 이름 (null-terminated, 가변 길이) */
};

/* ============================================================
 * 8. Base Relocation (가변 길이)
 * ============================================================
 *
 * PE가 ImageBase가 아닌 다른 주소에 로드될 때 필요.
 * 코드/데이터에 하드코딩된 절대 주소를 수정.
 *
 * 리로케이션 블록 구조:
 *   [블록 헤더: VirtualAddress(4) + SizeOfBlock(4)]
 *   [엔트리: 2바이트씩, 상위 4비트=타입, 하위 12비트=오프셋]
 *   [엔트리...]
 *
 * 예: 블록의 VirtualAddress=0x1000이고 엔트리=0xA042이면
 *   타입 = 0xA = IMAGE_REL_BASED_DIR64 (64비트 주소 수정)
 *   오프셋 = 0x042
 *   수정할 주소 = base + 0x1000 + 0x042
 *   해당 위치의 8바이트 값에 delta를 더함
 *
 * ELF는 이 대신 PIC (Position-Independent Code)를 사용.
 * PIC는 상대 주소만 사용하므로 리로케이션이 불필요.
 * 하지만 성능 오버헤드가 있음 (GOT 간접 참조).
 * PE는 절대 주소를 쓰고 리로케이션으로 수정하는 방식.
 */
struct __attribute__((packed)) IMAGE_BASE_RELOCATION {
	uint32_t VirtualAddress;	/* 페이지 RVA (4KB 단위) */
	uint32_t SizeOfBlock;		/* 블록 전체 크기 (헤더 + 엔트리) */
	/* 이 뒤에 uint16_t 엔트리 배열이 따라옴 */
};

/* Relocation 타입 (엔트리의 상위 4비트) */
#define IMAGE_REL_BASED_ABSOLUTE	0	/* 패딩 (무시) */
#define IMAGE_REL_BASED_HIGHLOW		3	/* 32비트 주소 수정 (PE32) */
#define IMAGE_REL_BASED_DIR64		10	/* ★ 64비트 주소 수정 (PE32+) */

#endif /* CITC_PE_H */
