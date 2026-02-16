/*
 * desktop_entry.h - .desktop 파일 파서 (header-only)
 * =====================================================
 *
 * .desktop 파일이란?
 *   Linux 데스크탑 환경에서 앱 정보를 정의하는 표준 형식.
 *   freedesktop.org의 Desktop Entry Specification이 정의.
 *   GNOME, KDE, XFCE 등 모든 Linux 데스크탑이 사용.
 *
 *   파일 위치: /usr/share/applications/ (*.desktop)
 *
 *   Windows 대응: .lnk (바로가기) 파일
 *   macOS 대응: .app 번들의 Info.plist
 *
 * .desktop 파일 형식 (INI 스타일):
 *
 *   [Desktop Entry]
 *   Name=Terminal
 *   Exec=/usr/bin/citcterm
 *   Icon=terminal
 *   Categories=System;
 *   Type=Application
 *
 * 이 파서가 지원하는 키:
 *   Name=  → 표시 이름 (태스크바 버튼 텍스트)
 *   Exec=  → 실행 파일 경로 (fork+exec 대상)
 *   Icon=  → 아이콘 이름 (미래용, 현재 미사용)
 *
 * 왜 header-only?
 *   citcshell.c에서만 사용하므로 .c 파일을 따로 만들 이유가 없음.
 *   cdp_client.h와 동일한 패턴.
 */

#ifndef CITC_DESKTOP_ENTRY_H
#define CITC_DESKTOP_ENTRY_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* .desktop 파일이 저장된 디렉토리 */
#define DESKTOP_DIR "/usr/share/applications"

/* 최대 로드 가능한 .desktop 항목 수 */
#define MAX_DESKTOP_ENTRIES 16

/* ============================================================
 * .desktop 파일 항목
 * ============================================================
 *
 * 하나의 .desktop 파일에서 파싱한 정보.
 *
 * 주의: 문자열은 구조체 내부에 저장 (static storage).
 * 포인터가 아닌 배열이므로 메모리 관리 불필요.
 * 이는 citcshell의 button 구조체가 const char* 포인터를
 * 가리키므로 중요: 이 배열이 사라지면 dangling pointer!
 * → entries 배열을 전역/정적으로 유지해야 함.
 */
struct desktop_entry {
	char name[64];        /* Name= 표시 이름 */
	char exec[256];       /* Exec= 실행 경로 */
	char icon[64];        /* Icon= 아이콘 (미래용) */
	int valid;            /* 1이면 유효한 항목 */
};

/* ============================================================
 * 단일 .desktop 파일 파싱
 * ============================================================
 *
 * config.c의 설정 파서와 동일한 패턴:
 *   한 줄씩 읽기 → '=' 찾기 → key/value 분리 → 매칭
 *
 * 차이점:
 *   - [Desktop Entry] 섹션 헤더를 확인
 *   - config.c는 커스텀 형식, 여기는 freedesktop 표준
 */
static int parse_desktop_file(const char *path, struct desktop_entry *entry)
{
	FILE *fp;
	char line[512];
	int in_entry = 0;

	memset(entry, 0, sizeof(*entry));

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		/* 줄 끝 개행 제거 */
		char *nl = strchr(line, '\n');

		if (nl)
			*nl = '\0';

		/* 빈 줄, 주석 건너뛰기 */
		if (line[0] == '\0' || line[0] == '#')
			continue;

		/* [Desktop Entry] 섹션 확인 */
		if (strcmp(line, "[Desktop Entry]") == 0) {
			in_entry = 1;
			continue;
		}

		/* 다른 섹션이 시작되면 중단 */
		if (line[0] == '[') {
			if (in_entry)
				break;  /* [Desktop Entry] 이후의 다른 섹션 */
			continue;
		}

		/* [Desktop Entry] 섹션 밖이면 건너뛰기 */
		if (!in_entry)
			continue;

		/* key=value 분리 */
		char *eq = strchr(line, '=');

		if (!eq)
			continue;

		*eq = '\0';
		char *key = line;
		char *value = eq + 1;

		/* 키 매칭 */
		if (strcmp(key, "Name") == 0) {
			snprintf(entry->name, sizeof(entry->name),
				 "%s", value);
		} else if (strcmp(key, "Exec") == 0) {
			snprintf(entry->exec, sizeof(entry->exec),
				 "%s", value);
		} else if (strcmp(key, "Icon") == 0) {
			snprintf(entry->icon, sizeof(entry->icon),
				 "%s", value);
		}
		/* 다른 키는 무시 (Type, Categories 등) */
	}

	fclose(fp);

	/* Name과 Exec가 있으면 유효 */
	if (entry->name[0] && entry->exec[0]) {
		entry->valid = 1;
		return 0;
	}

	return -1;
}

/* ============================================================
 * 디렉토리의 모든 .desktop 파일 로드
 * ============================================================
 *
 * config_load_services()와 동일한 패턴:
 *   opendir → readdir → 확장자 확인 → 파싱
 *
 * 반환: 로드된 항목 수
 */
static int load_desktop_entries(struct desktop_entry *entries, int max)
{
	DIR *dir;
	struct dirent *ent;
	int count = 0;

	dir = opendir(DESKTOP_DIR);
	if (!dir)
		return 0;

	while ((ent = readdir(dir)) != NULL && count < max) {
		/* .desktop 확장자 확인 */
		const char *dot = strrchr(ent->d_name, '.');

		if (!dot || strcmp(dot, ".desktop") != 0)
			continue;

		/* 전체 경로 구성 */
		char path[512];

		snprintf(path, sizeof(path), "%s/%s",
			 DESKTOP_DIR, ent->d_name);

		/* 파싱 */
		if (parse_desktop_file(path, &entries[count]) == 0)
			count++;
	}

	closedir(dir);
	return count;
}

#endif /* CITC_DESKTOP_ENTRY_H */
