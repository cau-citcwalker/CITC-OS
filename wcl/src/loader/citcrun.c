/*
 * citcrun — CITC OS Windows PE Loader
 * =====================================
 *
 * Windows .exe 파일을 Linux에서 로드하고 실행합니다.
 * WCL (Windows Compatibility Layer)의 첫 번째 컴포넌트.
 *
 * 이것이 Wine의 핵심 원리입니다:
 *   1. PE 파일의 헤더를 읽어 구조를 파악
 *   2. 섹션들을 메모리에 매핑 (mmap)
 *   3. 베이스 리로케이션 적용 (주소 보정)
 *   4. 임포트된 Windows API를 Linux 함수로 연결
 *   5. 엔트리포인트 호출 → Windows 프로그램 실행!
 *
 * Linux 커널의 ELF 로더와 비교:
 *   커널의 load_elf_binary()가 ELF를 로드하듯이,
 *   citcrun이 PE를 로드합니다.
 *   차이점: ELF 로더는 커널 안에서 동작하고,
 *   citcrun은 유저스페이스 프로그램입니다.
 *
 * 사용법:
 *   citcrun hello.exe          # 로드 + 실행
 *   citcrun --info hello.exe   # PE 헤더 정보만 출력
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <strings.h>	/* strcasecmp */

#include "../../include/pe.h"
#include "../dlls/kernel32/kernel32.h"
#include "../dlls/user32/user32.h"
#include "../dlls/gdi32/gdi32.h"
#include "../dlls/dxgi/dxgi.h"
#include "../dlls/d3d11/d3d11.h"
#include "../ntemu/ntdll.h"
#include "../ntemu/registry.h"

/* ============================================================
 * 1. PE 파일 읽기 & 검증
 * ============================================================ */

/*
 * DOS 헤더 읽기
 *
 * 모든 PE 파일은 DOS 헤더로 시작합니다.
 * "MZ" 시그니처를 확인하고 PE 헤더 오프셋(e_lfanew)을 읽습니다.
 */
static int pe_read_dos_header(int fd, struct IMAGE_DOS_HEADER *dos)
{
	if (pread(fd, dos, sizeof(*dos), 0) != sizeof(*dos)) {
		fprintf(stderr, "  오류: DOS 헤더 읽기 실패\n");
		return -1;
	}

	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		fprintf(stderr, "  오류: MZ 시그니처 없음 (0x%04X != 0x5A4D)\n",
			dos->e_magic);
		return -1;
	}

	return 0;
}

/*
 * PE/COFF 헤더 읽기
 *
 * DOS 헤더의 e_lfanew가 가리키는 위치에서:
 *   1. PE 시그니처("PE\0\0") 확인
 *   2. COFF File Header 읽기
 *   3. Optional Header (PE32+) 읽기
 */
static int pe_read_nt_headers(int fd,
			      const struct IMAGE_DOS_HEADER *dos,
			      struct IMAGE_FILE_HEADER *file_hdr,
			      struct IMAGE_OPTIONAL_HEADER64 *opt_hdr)
{
	uint32_t offset = dos->e_lfanew;

	/* PE 시그니처 확인 */
	uint32_t sig;

	if (pread(fd, &sig, 4, offset) != 4) {
		fprintf(stderr, "  오류: PE 시그니처 읽기 실패\n");
		return -1;
	}
	if (sig != IMAGE_NT_SIGNATURE) {
		fprintf(stderr, "  오류: PE 시그니처 없음 (0x%08X != 0x4550)\n",
			sig);
		return -1;
	}
	offset += 4;

	/* File Header */
	if (pread(fd, file_hdr, sizeof(*file_hdr), offset) !=
	    (ssize_t)sizeof(*file_hdr)) {
		fprintf(stderr, "  오류: File Header 읽기 실패\n");
		return -1;
	}
	offset += sizeof(*file_hdr);

	/* 64비트 PE만 지원 */
	if (file_hdr->Machine != IMAGE_FILE_MACHINE_AMD64) {
		fprintf(stderr, "  오류: x86_64만 지원 (Machine=0x%04X)\n",
			file_hdr->Machine);
		return -1;
	}

	/* Optional Header (PE32+) */
	if (pread(fd, opt_hdr, sizeof(*opt_hdr), offset) !=
	    (ssize_t)sizeof(*opt_hdr)) {
		fprintf(stderr, "  오류: Optional Header 읽기 실패\n");
		return -1;
	}

	if (opt_hdr->Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
		fprintf(stderr, "  오류: PE32+만 지원 (Magic=0x%04X)\n",
			opt_hdr->Magic);
		return -1;
	}

	return 0;
}

/*
 * 섹션 헤더 읽기
 *
 * Optional Header 바로 뒤에 섹션 헤더 배열이 위치.
 */
static int pe_read_sections(int fd,
			    const struct IMAGE_DOS_HEADER *dos,
			    const struct IMAGE_FILE_HEADER *file_hdr,
			    struct IMAGE_SECTION_HEADER *sections)
{
	/* 섹션 헤더 시작 위치:
	 *   e_lfanew + 4 (PE sig) + sizeof(File Header)
	 *   + SizeOfOptionalHeader */
	uint32_t offset = dos->e_lfanew + 4
			  + sizeof(struct IMAGE_FILE_HEADER)
			  + file_hdr->SizeOfOptionalHeader;
	size_t total = (size_t)file_hdr->NumberOfSections
		       * sizeof(struct IMAGE_SECTION_HEADER);

	if (pread(fd, sections, total, offset) != (ssize_t)total) {
		fprintf(stderr, "  오류: 섹션 헤더 읽기 실패\n");
		return -1;
	}

	return 0;
}

/* ============================================================
 * 2. PE 정보 출력 (교육용)
 * ============================================================ */

static void pe_dump_info(const struct IMAGE_DOS_HEADER *dos,
			 const struct IMAGE_FILE_HEADER *file_hdr,
			 const struct IMAGE_OPTIONAL_HEADER64 *opt_hdr,
			 const struct IMAGE_SECTION_HEADER *sections,
			 uint16_t num_sections)
{
	printf("\n=== PE Header Info ===\n\n");

	/* DOS Header */
	printf("DOS Header:\n");
	printf("  e_magic:  0x%04X (\"MZ\")\n", dos->e_magic);
	printf("  e_lfanew: 0x%08X (PE 헤더 오프셋)\n", dos->e_lfanew);

	/* File Header */
	printf("\nFile Header (COFF):\n");
	printf("  Machine:          0x%04X (%s)\n", file_hdr->Machine,
	       file_hdr->Machine == IMAGE_FILE_MACHINE_AMD64 ? "x86_64" :
	       "unknown");
	printf("  NumberOfSections: %u\n", file_hdr->NumberOfSections);
	printf("  Characteristics:  0x%04X", file_hdr->Characteristics);
	if (file_hdr->Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE)
		printf(" [EXECUTABLE]");
	if (file_hdr->Characteristics & IMAGE_FILE_DLL)
		printf(" [DLL]");
	printf("\n");

	/* Optional Header */
	printf("\nOptional Header (PE32+):\n");
	printf("  Magic:              0x%04X (PE32+)\n", opt_hdr->Magic);
	printf("  AddressOfEntryPoint: 0x%08X (RVA)\n",
	       opt_hdr->AddressOfEntryPoint);
	printf("  ImageBase:          0x%016lX\n",
	       (unsigned long)opt_hdr->ImageBase);
	printf("  SectionAlignment:   0x%X (%u)\n",
	       opt_hdr->SectionAlignment, opt_hdr->SectionAlignment);
	printf("  FileAlignment:      0x%X (%u)\n",
	       opt_hdr->FileAlignment, opt_hdr->FileAlignment);
	printf("  SizeOfImage:        0x%X (%u bytes)\n",
	       opt_hdr->SizeOfImage, opt_hdr->SizeOfImage);
	printf("  Subsystem:          %u (%s)\n", opt_hdr->Subsystem,
	       opt_hdr->Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI ? "Console" :
	       opt_hdr->Subsystem == IMAGE_SUBSYSTEM_WINDOWS_GUI ? "GUI" :
	       "Unknown");

	/* Sections */
	printf("\nSections (%u):\n", num_sections);
	printf("  %-8s  %-10s  %-10s  %-10s  %-10s  %s\n",
	       "Name", "VirtAddr", "VirtSize", "RawOff", "RawSize", "Flags");

	for (int i = 0; i < num_sections; i++) {
		const struct IMAGE_SECTION_HEADER *s = &sections[i];
		char name[9] = {0};

		memcpy(name, s->Name, 8);

		printf("  %-8s  0x%08X  0x%08X  0x%08X  0x%08X  ",
		       name, s->VirtualAddress, s->VirtualSize,
		       s->PointerToRawData, s->SizeOfRawData);

		if (s->Characteristics & IMAGE_SCN_MEM_READ) printf("R");
		else printf("-");
		if (s->Characteristics & IMAGE_SCN_MEM_WRITE) printf("W");
		else printf("-");
		if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) printf("X");
		else printf("-");
		printf("\n");
	}

	/* Import directory info */
	if (opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size > 0)
		printf("\nImport Table: RVA=0x%08X  Size=%u bytes\n",
		       opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress,
		       opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size);

	/* Relocation info */
	if (opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size > 0)
		printf("Reloc Table:  RVA=0x%08X  Size=%u bytes\n",
		       opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress,
		       opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);

	printf("\n");
}

/* ============================================================
 * 3. 섹션 매핑 (mmap)
 * ============================================================
 *
 * PE 섹션을 메모리에 매핑하는 과정:
 *
 *   파일 (디스크):                    메모리:
 *   ┌──────────────┐               ┌──────────────┐ base
 *   │ Headers      │               │ Headers      │
 *   ├──────────────┤               ├──────────────┤ base + 0x1000
 *   │ .text (코드)  │  ──mmap──→   │ .text [R-X]  │
 *   ├──────────────┤               ├──────────────┤ base + 0x2000
 *   │ .rdata (상수) │  ──mmap──→   │ .rdata [R--] │
 *   ├──────────────┤               ├──────────────┤ base + 0x3000
 *   │ .reloc (재배치)│  ──mmap──→   │ .reloc [R--] │
 *   └──────────────┘               └──────────────┘
 *
 * Linux의 ELF 로더도 같은 작업을 합니다:
 *   load_elf_binary() → elf_map() → do_mmap()
 */
static int pe_map_sections(int fd,
			   const struct IMAGE_OPTIONAL_HEADER64 *opt_hdr,
			   const struct IMAGE_SECTION_HEADER *sections,
			   uint16_t num_sections,
			   uint8_t **base_out)
{
	/*
	 * 1단계: 전체 이미지 크기만큼 주소 공간 예약
	 *
	 * PROT_NONE으로 매핑하여 주소만 확보.
	 * 나중에 각 섹션을 MAP_FIXED로 덮어씀.
	 *
	 * ImageBase에 로드하려고 시도하지 않음 — 항상 ASLR 방식.
	 * 이후 리로케이션으로 주소를 보정.
	 */
	size_t image_size = opt_hdr->SizeOfImage;
	uint8_t *base = mmap(NULL, image_size,
			     PROT_NONE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (base == MAP_FAILED) {
		perror("  mmap(reserve)");
		return -1;
	}

	/*
	 * 2단계: 헤더 매핑 (SizeOfHeaders 크기)
	 *
	 * 일부 PE는 헤더 영역의 데이터를 RVA로 참조합니다.
	 */
	size_t hdr_size = opt_hdr->SizeOfHeaders;

	if (mmap(base, hdr_size,
		 PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
		 -1, 0) == MAP_FAILED) {
		perror("  mmap(headers)");
		munmap(base, image_size);
		return -1;
	}
	if (pread(fd, base, hdr_size, 0) != (ssize_t)hdr_size) {
		fprintf(stderr, "  오류: 헤더 읽기 실패\n");
		munmap(base, image_size);
		return -1;
	}

	/*
	 * 3단계: 각 섹션 매핑
	 *
	 * 처음에는 PROT_READ|PROT_WRITE로 매핑 (리로케이션/임포트 패치용).
	 * 나중에 pe_set_section_protection()으로 최종 보호 설정.
	 */
	for (int i = 0; i < num_sections; i++) {
		const struct IMAGE_SECTION_HEADER *s = &sections[i];

		if (s->SizeOfRawData == 0)
			continue;

		uint8_t *addr = base + s->VirtualAddress;
		size_t map_size = s->VirtualSize;

		if (map_size == 0)
			map_size = s->SizeOfRawData;

		/* 페이지 정렬 */
		size_t aligned = (map_size + 4095) & ~(size_t)4095;

		if (mmap(addr, aligned,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
			 -1, 0) == MAP_FAILED) {
			char name[9] = {0};

			memcpy(name, s->Name, 8);
			fprintf(stderr, "  mmap(%s) 실패\n", name);
			munmap(base, image_size);
			return -1;
		}

		/* 파일에서 섹션 데이터 읽기 */
		size_t read_size = s->SizeOfRawData;

		if (read_size > aligned)
			read_size = aligned;
		if (pread(fd, addr, read_size, s->PointerToRawData) !=
		    (ssize_t)read_size) {
			char name[9] = {0};

			memcpy(name, s->Name, 8);
			fprintf(stderr, "  pread(%s) 실패\n", name);
			munmap(base, image_size);
			return -1;
		}
	}

	*base_out = base;
	return 0;
}

/* ============================================================
 * 4. 베이스 리로케이션
 * ============================================================
 *
 * PE가 ImageBase(기본 주소)가 아닌 곳에 로드되면,
 * 코드/데이터에 하드코딩된 절대 주소를 보정해야 합니다.
 *
 *   delta = 실제_로드_주소 - ImageBase
 *
 * 예: ImageBase=0x140000000, 실제=0x7f0000000000
 *   코드에 mov rax, 0x140001234 가 있으면
 *   0x140001234 + delta = 0x7f0000001234 로 수정
 *
 * ELF는 이 문제를 PIC(Position-Independent Code)로 해결:
 *   상대 주소만 사용하므로 리로케이션이 불필요.
 *   대신 GOT/PLT 간접 참조 오버헤드 있음.
 */
static int pe_apply_relocations(uint8_t *base,
				const struct IMAGE_OPTIONAL_HEADER64 *opt_hdr,
				int64_t delta)
{
	if (delta == 0) {
		printf("  delta=0 (ImageBase에 로드됨) — 리로케이션 불필요\n");
		return 0;
	}

	struct IMAGE_DATA_DIRECTORY reloc_dir =
		opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];

	if (reloc_dir.Size == 0 || reloc_dir.VirtualAddress == 0) {
		/*
		 * .reloc 섹션이 없는 PE 파일.
		 * x86_64에서는 RIP-relative 주소 지정을 사용하므로
		 * 절대 주소가 없어 리로케이션 없이도 동작할 수 있다.
		 * (MinGW -nostdlib로 빌드한 간단한 프로그램이 이 경우)
		 */
		printf("  .reloc 없음 — RIP-relative 코드로 간주 (skip)\n");
		return 0;
	}

	uint8_t *reloc_ptr = base + reloc_dir.VirtualAddress;
	uint8_t *reloc_end = reloc_ptr + reloc_dir.Size;
	int fixup_count = 0;

	while (reloc_ptr < reloc_end) {
		struct IMAGE_BASE_RELOCATION *block =
			(struct IMAGE_BASE_RELOCATION *)reloc_ptr;

		if (block->SizeOfBlock == 0)
			break;

		/* 엔트리 배열은 블록 헤더(8바이트) 뒤에 위치 */
		uint16_t *entries = (uint16_t *)(reloc_ptr + 8);
		int num_entries = (int)(block->SizeOfBlock - 8) / 2;

		for (int i = 0; i < num_entries; i++) {
			uint16_t entry = entries[i];
			int type = entry >> 12;
			int offset = entry & 0xFFF;
			uint8_t *target = base + block->VirtualAddress + offset;

			switch (type) {
			case IMAGE_REL_BASED_ABSOLUTE:
				/* 패딩 엔트리 — 무시 */
				break;

			case IMAGE_REL_BASED_DIR64:
				/*
				 * 64비트 주소 수정
				 * target이 가리키는 8바이트 값에 delta를 더함
				 */
				*(int64_t *)target += delta;
				fixup_count++;
				break;

			case IMAGE_REL_BASED_HIGHLOW:
				/* 32비트 주소 수정 (PE32용, 참고로 포함) */
				*(int32_t *)target += (int32_t)delta;
				fixup_count++;
				break;

			default:
				fprintf(stderr,
					"  경고: 미지원 리로케이션 타입 %d\n",
					type);
				break;
			}
		}

		reloc_ptr += block->SizeOfBlock;
	}

	printf("  delta=0x%lX (%d개 fixup 적용)\n",
	       (unsigned long)delta, fixup_count);
	return 0;
}

/*
 * 미구현 함수 — catch-all 스텁
 * 임포트 해석에서 매칭되지 않은 함수가 호출되면 여기로 옴.
 * (kernel32 외의 DLL 함수나, 아직 구현하지 않은 함수용)
 */
__attribute__((ms_abi))
static void stub_unimplemented(void)
{
	fprintf(stderr, "\n오류: 미구현 Windows API 호출!\n");
	_exit(1);
}

/* ============================================================
 * 5. 임포트 해석
 * ============================================================
 *
 * Windows .exe가 DLL 함수를 호출하는 방식:
 *
 *   .exe 코드:  call [IAT + 0x10]    ← IAT를 통한 간접 호출
 *   IAT 초기값: 0x00000000           ← 로더가 채워야 함
 *   로더:       IAT[0x10] = &stub_WriteFile  ← 스텁 주소 씀!
 *   결과:       call stub_WriteFile   ← Linux 코드 실행됨
 *
 * ELF의 GOT/PLT와 같은 원리입니다.
 *   PLT[n]: jmp *GOT[n]
 *   GOT[n] = <해석된 함수 주소>
 */

/*
 * 스텁 테이블에서 함수 찾기
 *
 * kernel32_stub_table은 kernel32.c에서 정의됨.
 * 나중에 user32, gdi32 등의 DLL이 추가되면
 * 여러 테이블을 순차적으로 검색하도록 확장 가능.
 */
/*
 * 다중 DLL 스텁 테이블 검색
 *
 * 모든 등록된 stub table을 순차적으로 검색.
 * 새 DLL 추가 시 이 배열에 테이블을 추가하면 됨.
 */
static struct stub_entry *all_stub_tables[] = {
	kernel32_stub_table,
	user32_stub_table,
	gdi32_stub_table,
	dxgi_stub_table,
	d3d11_stub_table,
	ntdll_stub_table,
	advapi32_stub_table,
	NULL
};

static void *find_stub(const char *dll, const char *func)
{
	for (int t = 0; all_stub_tables[t]; t++) {
		struct stub_entry *table = all_stub_tables[t];

		for (int i = 0; table[i].dll_name; i++) {
			if (strcasecmp(table[i].dll_name, dll) == 0 &&
			    strcmp(table[i].func_name, func) == 0)
				return table[i].func_ptr;
		}
	}
	return NULL;
}

/*
 * 임포트 테이블 처리
 *
 * Import Directory → 각 DLL의 Import Descriptor
 *   → ILT에서 함수 이름 읽기
 *   → 스텁 테이블에서 매칭
 *   → IAT에 스텁 주소 쓰기
 */
static int pe_resolve_imports(uint8_t *base,
			      const struct IMAGE_OPTIONAL_HEADER64 *opt_hdr)
{
	struct IMAGE_DATA_DIRECTORY import_dir =
		opt_hdr->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

	if (import_dir.Size == 0 || import_dir.VirtualAddress == 0) {
		printf("  임포트 없음\n");
		return 0;
	}

	struct IMAGE_IMPORT_DESCRIPTOR *desc =
		(struct IMAGE_IMPORT_DESCRIPTOR *)(base + import_dir.VirtualAddress);
	int resolved = 0;
	int unresolved = 0;

	/* Import Descriptor 배열 순회 (마지막은 all-zero) */
	while (desc->Name != 0) {
		const char *dll_name = (const char *)(base + desc->Name);

		printf("  %s:\n", dll_name);

		/*
		 * ILT (Import Lookup Table)와 IAT를 동시에 순회.
		 * ILT에서 함수 이름을 읽고, IAT에 주소를 씀.
		 *
		 * OriginalFirstThunk = ILT (원본, 읽기 전용)
		 * FirstThunk = IAT (로더가 수정)
		 */
		uint64_t *ilt = (uint64_t *)(base + desc->OriginalFirstThunk);
		uint64_t *iat = (uint64_t *)(base + desc->FirstThunk);

		/* ILT가 없으면 IAT에서 직접 읽기 */
		if (desc->OriginalFirstThunk == 0)
			ilt = iat;

		for (int i = 0; ilt[i] != 0; i++) {
			if (ilt[i] & IMAGE_ORDINAL_FLAG64) {
				/* 서수(ordinal)로 임포트 — 미지원 */
				uint16_t ordinal = (uint16_t)(ilt[i] & 0xFFFF);

				printf("    #%u (ordinal) → 미지원\n", ordinal);
				iat[i] = (uint64_t)(uintptr_t)stub_unimplemented;
				unresolved++;
				continue;
			}

			/* 이름으로 임포트 */
			struct IMAGE_IMPORT_BY_NAME *ibn =
				(struct IMAGE_IMPORT_BY_NAME *)
				(base + (uint32_t)ilt[i]);
			const char *func_name = ibn->Name;

			void *stub = find_stub(dll_name, func_name);

			if (stub) {
				iat[i] = (uint64_t)(uintptr_t)stub;
				printf("    %s → stub OK\n", func_name);
				resolved++;
			} else {
				iat[i] = (uint64_t)(uintptr_t)stub_unimplemented;
				printf("    %s → 미구현!\n", func_name);
				unresolved++;
			}
		}

		desc++;
	}

	printf("  총 %d개 해석, %d개 미구현\n", resolved, unresolved);

	if (unresolved > 0)
		printf("  경고: 미구현 함수 호출 시 프로그램이 중단됩니다.\n");

	return 0;
}

/* ============================================================
 * 6. 섹션 메모리 보호 설정
 * ============================================================
 *
 * 리로케이션과 임포트 패치가 끝나면 최종 보호 속성을 적용.
 *
 * 매핑 시에는 PROT_READ|PROT_WRITE로 열어뒀다가 (패치를 위해),
 * 이제 PE 헤더에 명시된 원래 권한으로 잠근다:
 *
 *   .text  → R-X  (코드 실행 가능, 쓰기 불가)
 *   .rdata → R--  (읽기 전용)
 *   .idata → R--  (IAT 패치 완료 후 잠금)
 *
 * 이걸 안 하면 .text에 실행 권한이 없어서 entry() 호출 시
 * Segmentation fault가 발생한다! (NX bit 보호)
 */
static void pe_set_section_protection(uint8_t *base,
				      const struct IMAGE_SECTION_HEADER *sections,
				      uint16_t num_sections)
{
	for (int i = 0; i < num_sections; i++) {
		const struct IMAGE_SECTION_HEADER *s = &sections[i];

		if (s->VirtualSize == 0 && s->SizeOfRawData == 0)
			continue;

		size_t size = s->VirtualSize;

		if (size == 0)
			size = s->SizeOfRawData;
		size = (size + 4095) & ~(size_t)4095;

		int prot = 0;

		if (s->Characteristics & IMAGE_SCN_MEM_READ)
			prot |= PROT_READ;
		if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
			prot |= PROT_WRITE;
		if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE)
			prot |= PROT_EXEC;

		/* 최소한 PROT_READ는 보장 */
		if (prot == 0)
			prot = PROT_READ;

		mprotect(base + s->VirtualAddress, size, prot);
	}
}

/* ============================================================
 * 7. 엔트리포인트 호출
 * ============================================================ */

/*
 * PE의 엔트리포인트를 호출합니다.
 *
 * 이 순간, citcrun이 "OS"가 됩니다:
 *   Linux 커널이 ELF의 _start를 호출하듯이,
 *   citcrun이 PE의 엔트리포인트를 호출합니다.
 *
 * ms_abi 호출 규약으로 호출해야 합니다.
 * (Windows .exe는 Microsoft x64 ABI로 컴파일됨)
 */
typedef void __attribute__((ms_abi)) (*pe_entry_fn)(void);

/* ============================================================
 * 8. 메인 함수 (엔트리포인트)
 * ============================================================ */

static void usage(const char *prog)
{
	printf("사용법: %s [옵션] <파일.exe>\n\n", prog);
	printf("옵션:\n");
	printf("  --info    PE 헤더 정보만 출력 (실행 안 함)\n");
	printf("  --help    이 도움말 표시\n\n");
	printf("예시:\n");
	printf("  %s hello.exe          # Windows 프로그램 실행\n", prog);
	printf("  %s --info hello.exe   # PE 구조 분석\n", prog);
}

int main(int argc, char *argv[])
{
	int info_only = 0;
	const char *exe_path = NULL;

	/* 인자 파싱 */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--info") == 0)
			info_only = 1;
		else if (strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else
			exe_path = argv[i];
	}

	if (!exe_path) {
		usage(argv[0]);
		return 1;
	}

	printf("\n=== CITC PE Loader ===\n\n");
	printf("파일: %s\n\n", exe_path);

	/* 파일 열기 */
	int fd = open(exe_path, O_RDONLY);

	if (fd < 0) {
		perror(exe_path);
		return 1;
	}

	/* 1. DOS 헤더 */
	printf("[1/5] DOS 헤더 읽기...");
	struct IMAGE_DOS_HEADER dos;

	if (pe_read_dos_header(fd, &dos) < 0) {
		close(fd);
		return 1;
	}
	printf(" MZ OK\n");

	/* 2. PE 헤더 */
	printf("[2/5] PE 헤더 읽기...");
	struct IMAGE_FILE_HEADER file_hdr;
	struct IMAGE_OPTIONAL_HEADER64 opt_hdr;

	if (pe_read_nt_headers(fd, &dos, &file_hdr, &opt_hdr) < 0) {
		close(fd);
		return 1;
	}
	printf(" PE32+ (x86_64) OK\n");

	/* 섹션 헤더 읽기 */
	int num_sections = file_hdr.NumberOfSections;
	struct IMAGE_SECTION_HEADER sections[num_sections];

	if (pe_read_sections(fd, &dos, &file_hdr, sections) < 0) {
		close(fd);
		return 1;
	}

	/* --info 모드: 헤더 덤프만 */
	if (info_only) {
		pe_dump_info(&dos, &file_hdr, &opt_hdr, sections,
			     (uint16_t)num_sections);
		close(fd);
		return 0;
	}

	/* 3. 섹션 매핑 */
	printf("[3/5] 섹션 매핑 (%d개)...\n", num_sections);
	for (int i = 0; i < num_sections; i++) {
		char name[9] = {0};

		memcpy(name, sections[i].Name, 8);
		printf("  %-8s RVA=0x%04X  Size=0x%04X  [%c%c%c]\n",
		       name,
		       sections[i].VirtualAddress,
		       sections[i].SizeOfRawData,
		       (sections[i].Characteristics & IMAGE_SCN_MEM_READ)
		       ? 'R' : '-',
		       (sections[i].Characteristics & IMAGE_SCN_MEM_WRITE)
		       ? 'W' : '-',
		       (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)
		       ? 'X' : '-');
	}

	uint8_t *base;

	if (pe_map_sections(fd, &opt_hdr, sections,
			    (uint16_t)num_sections, &base) < 0) {
		close(fd);
		return 1;
	}

	close(fd); /* 파일은 더 이상 필요 없음 */

	/* 4. 리로케이션 */
	printf("[4/5] 리로케이션 적용...\n");
	int64_t delta = (int64_t)(uintptr_t)base - (int64_t)opt_hdr.ImageBase;

	if (pe_apply_relocations(base, &opt_hdr, delta) < 0) {
		munmap(base, opt_hdr.SizeOfImage);
		return 1;
	}

	/* 5. 임포트 해석 */
	printf("[5/5] 임포트 해석...\n");
	kernel32_init();  /* NT + Object Manager + 레지스트리 초기화 */
	user32_init();    /* 윈도우 테이블 + self-pipe 초기화 */
	kernel32_set_cmdline(exe_path); /* GetCommandLineA용 */
	if (pe_resolve_imports(base, &opt_hdr) < 0) {
		munmap(base, opt_hdr.SizeOfImage);
		return 1;
	}

	/* 섹션 보호 속성 적용 (.text → R-X, .rdata → R-- 등) */
	pe_set_section_protection(base, sections, (uint16_t)num_sections);

	/* 엔트리포인트 호출 */
	uint8_t *entry_addr = base + opt_hdr.AddressOfEntryPoint;

	printf("\n>>> 엔트리포인트 실행 (RVA=0x%X) >>>\n",
	       opt_hdr.AddressOfEntryPoint);

	pe_entry_fn entry = (pe_entry_fn)entry_addr;

	entry();

	/*
	 * ExitProcess가 _exit()를 호출하므로 여기에 도달하지 않아야 함.
	 * 만약 도달하면 정상 종료로 처리.
	 */
	printf("\n>>> 엔트리포인트 반환 <<<\n");
	munmap(base, opt_hdr.SizeOfImage);

	return 0;
}
