// Part of Neogeo_MiSTer
// (C) 2019 Sean 'furrtek' Gonsalves

#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>   // clock_gettime, CLOCK_REALTIME
#include <sys/mman.h>
#include "loader.h"
#include "../../sxmlc.h"
#include "../../user_io.h"
#include "../../osd.h"
#include "../../menu.h"

static inline void spr_convert(uint16_t* buf_in, uint16_t* buf_out, uint32_t size)
{
	/*
	In C ROMs, a word provides two bitplanes for an 8-pixel wide line
	They're used in pairs to provide 32 bits at once (all four bitplanes)
	For one sprite tile, bytes are used like this: ([...] represents one 8-pixel wide line)
	Even ROM					Odd ROM
	[  40 41  ][  00 01  ]		[  42 43  ][  02 03  ]
	[  44 45  ][  04 05  ]  	[  46 47  ][  06 07  ]
	[  48 49  ][  08 09  ]  	[  4A 4B  ][  0A 0B  ]
	[  4C 4D  ][  0C 0D  ]  	[  4E 4F  ][  0E 0F  ]
	[  50 51  ][  10 11  ]  	[  52 53  ][  12 13  ]
	...							...
	The data read for a given tile line (16 pixels) is always the same, only the rendering order of the pixels can change
	To take advantage of the SDRAM burst read feature, the data can be loaded so that all 16 pixels of a tile
	line can be read sequentially: () are 16-bit words, [] is the 4-word burst read
	[(40 41) (00 01) (42 43) (02 03)]
	[(44 45) (04 05) (46 47) (06 07)]...
	Word interleaving is done on the FPGA side to mix the two C ROMs data (even/odd)

	In:  FEDCBA9876 54321 0
	Out: FEDCBA9876 15432 0
	*/

	for (uint32_t i = 0; i < size; i++) buf_out[i] = buf_in[(i & ~0x1F) | ((i >> 1) & 0xF) | (((i & 1) ^ 1) << 4)];

	/*
	0 <- 20
	1 <- 21
	2 <- 00
	3 <- 01
	4 <- 22
	5 <- 23
	6 <- 02
	7 <- 03
	...

	00 -> 02
	01 -> 03
	02 -> 06
	03 -> 07
	...
	*/
}

static inline void spr_convert_skp(uint16_t* buf_in, uint16_t* buf_out, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++) buf_out[i << 1] = buf_in[(i & ~0x1F) | ((i >> 1) & 0xF) | (((i & 1) ^ 1) << 4)];
}

static inline void spr_convert_dbl(uint16_t* buf_in, uint16_t* buf_out, uint32_t size)
{
	for (uint32_t i = 0; i < size; i++) buf_out[i] = buf_in[(i & ~0x3F) | ((i ^ 1) & 1) | ((i >> 1) & 0x1E) | (((i & 2) ^ 2) << 4)];
}

static void fix_convert(uint8_t* buf_in, uint8_t* buf_out, uint32_t size)
{
	/*
	In S ROMs, a byte provides two pixels
	For one fix tile, bytes are used like this: ([...] represents a pair of pixels)
	[10][18][00][08]
	[11][19][01][09]
	[12][1A][02][0A]
	[13][1B][03][0B]
	[14][1C][04][0C]
	[15][1D][05][0D]
	[16][1E][06][0E]
	[17][1F][07][0F]
	The data read for a given tile line (8 pixels) is always the same
	To take advantage of the SDRAM burst read feature, the data can be loaded so that all 8 pixels of a tile
	line can be read sequentially: () are 16-bit words, [] is the 2-word burst read
	[(10 18) (00 08)]
	[(11 19) (01 09)]...

	In:  FEDCBA9876543210
	Out: FEDCBA9876510432
	*/
	for (uint32_t i = 0; i < size; i++) buf_out[i] = buf_in[(i & ~0x1F) | ((i >> 2) & 7) | ((i & 1) << 3) | (((i & 2) << 3) ^ 0x10)];
}

static char pchar[] = { 0x8C, 0x8E, 0x8F, 0x90, 0x91, 0x7F };

#define PROGRESS_CNT    10
#define PROGRESS_CHARS  (sizeof(pchar)/sizeof(pchar[0]))
#define PROGRESS_MAX    ((PROGRESS_CHARS*PROGRESS_CNT)-1)

static void neogeo_osd_progress(const char* name, unsigned int progress)
{
	static char progress_buf[64];
	memset(progress_buf, ' ', sizeof(progress_buf));

	if (progress > PROGRESS_MAX) progress = PROGRESS_MAX;
	char c = pchar[progress % PROGRESS_CHARS];
	progress /= PROGRESS_CHARS;

	strcpy(progress_buf, name);
	char *buf = progress_buf + strlen(progress_buf);
	*buf++ = ' ';

	for (unsigned int i = 0; i <= progress; i++) buf[i] = (i < progress) ? 0x7F : c;
	buf[PROGRESS_CNT] = 0;

	Info(progress_buf);
}

static uint32_t neogeo_file_tx(const char* path, const char* name, uint8_t neo_file_type, uint8_t index, uint32_t offset, uint32_t size)
{
	fileTYPE f = {};
	uint8_t buf[4096];	// Same in user_io_file_tx
	uint8_t buf_out[4096];
	static char name_buf[1024];

	sprintf(name_buf, "%s/%s", path, name);
	if (!FileOpen(&f, name_buf, 0)) return 0;
	if (!size && offset < f.size) size = f.size - offset;
	if (!size) return 0;

	uint32_t bytes2send = size;

	FileSeek(&f, offset, SEEK_SET);
	printf("Loading %s (offset %u, size %u, type %u) with index %u\n", name, offset, bytes2send, neo_file_type, index);

	// Put pairs of bitplanes in the correct order for the core
	if (neo_file_type == NEO_FILE_SPR && index != 15) index ^= 1;
	// set index byte
	user_io_set_index(index);

	// prepare transmission of new file
	EnableFpga();
	spi8(UIO_FILE_TX);
	spi8(0xff);
	DisableFpga();

	int progress = -1;

	while (bytes2send)
	{
		uint16_t chunk = (bytes2send > sizeof(buf)) ? sizeof(buf) : bytes2send;

		FileReadAdv(&f, buf, chunk);

		EnableFpga();
		spi8(UIO_FILE_TX_DAT);

		if (neo_file_type == NEO_FILE_RAW)
		{
			spi_write(buf, chunk, 1);
		}
		else if (neo_file_type == NEO_FILE_8BIT)
		{
			spi_write(buf, chunk, 0);
		}
		else
		{
			if (neo_file_type == NEO_FILE_FIX) fix_convert(buf, buf_out, sizeof(buf_out));
			else if (neo_file_type == NEO_FILE_SPR)
			{
				if (index == 15) spr_convert_dbl((uint16_t*)buf, (uint16_t*)buf_out, sizeof(buf_out)/2);
				else spr_convert((uint16_t*)buf, (uint16_t*)buf_out, sizeof(buf_out)/2);
			}

			spi_write(buf_out, chunk, 1);
		}

		DisableFpga();
		int new_progress = PROGRESS_MAX - ((((uint64_t)bytes2send)*PROGRESS_MAX) / size);
		if (progress != new_progress)
		{
			progress = new_progress;
			neogeo_osd_progress(name, progress);
		}
		bytes2send -= chunk;
	}

	FileClose(&f);

	// signal end of transmission
	EnableFpga();
	spi8(UIO_FILE_TX);
	spi8(0x00);
	DisableFpga();

	return size;
}

static uint8_t loadbuf[1024 * 1024];
static uint32_t load_crom_to_mem(const char* path, const char* name, uint8_t index, uint32_t offset, uint32_t size)
{
	fileTYPE f = {};
	static char name_buf[1024];

	sprintf(name_buf, "%s/%s", path, name);
	if (!FileOpen(&f, name_buf, 0)) return 0;
	if (!size && offset < f.size) size = f.size - offset;
	if (!size)
	{
		FileClose(&f);
		return 0;
	}

	int memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd == -1)
	{
		printf("Unable to open /dev/mem!\n");
		FileClose(&f);
		return 0;
	}

	size *= 2;

	FileSeek(&f, offset, SEEK_SET);
	printf("CROM %s (offset %u, size %u) with index %u\n", name, offset, size, index);

	// Put pairs of bitplanes in the correct order for the core
	int progress = -1;

	uint32_t remain = size;
	uint32_t map_addr = 0x38000000 + (((index - 64) >> 1) * 1024 * 1024);

	while (remain)
	{
		uint32_t partsz = remain;
		if (partsz > 1024 * 1024) partsz = 1024 * 1024;

		//printf("partsz=%d, map_addr=0x%X\n", partsz, map_addr);
		void *base = mmap(0, partsz, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, map_addr);
		if (base == (void *)-1)
		{
			printf("Unable to mmap (0x%X, %d)!\n", map_addr, partsz);
			close(memfd);
			FileClose(&f);
			return 0;
		}

		FileReadAdv(&f, loadbuf, partsz/2);
		spr_convert_skp((uint16_t*)loadbuf, ((uint16_t*)base) + ((index ^ 1) & 1), partsz / 4);

		int new_progress = PROGRESS_MAX - ((((uint64_t)(remain - partsz))*PROGRESS_MAX) / size);
		if (progress != new_progress)
		{
			progress = new_progress;
			neogeo_osd_progress(name, progress);
		}

		munmap(base, partsz);
		remain -= partsz;
		map_addr += partsz;
	}

	close(memfd);
	FileClose(&f);

	return map_addr - 0x38000000;
}

static uint32_t load_rom_to_mem(const char* path, const char* name, uint8_t neo_file_type, uint8_t index, uint32_t offset, uint32_t size, uint32_t expand)
{
	fileTYPE f = {};
	static char name_buf[1024];

	sprintf(name_buf, "%s/%s", path, name);
	if (!FileOpen(&f, name_buf, 0)) return 0;
	if (!size && offset < f.size) size = f.size - offset;
	if (!size)
	{
		FileClose(&f);
		return 0;
	}

	int memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd == -1)
	{
		printf("Unable to open /dev/mem!\n");
		FileClose(&f);
		return 0;
	}

	FileSeek(&f, offset, SEEK_SET);
	printf("ROM %s (offset %u, size %u, exp %u, type %u) with index %u\n", name, offset, size, expand, neo_file_type, index);

	int progress = -1;

	uint32_t remainf = size;

	if(expand) size = expand;
	uint32_t remain = size;

	uint32_t map_addr = 0x30000000 + (((index >= 16) && (index < 64)) ? (index - 16) * 0x80000 : (index == 9) ? 0x2000000 : 0x8000000);

	while (remain)
	{
		uint32_t partsz = remain;
		if (partsz > 1024 * 1024) partsz = 1024 * 1024;

		uint32_t partszf = remainf;
		if (partszf > 1024 * 1024) partszf = 1024 * 1024;

		//printf("partsz=%d, map_addr=0x%X\n", partsz, map_addr);
		void *base = mmap(0, partsz, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, map_addr);
		if (base == (void *)-1)
		{
			printf("Unable to mmap (0x%X, %d)!\n", map_addr, partsz);
			close(memfd);
			FileClose(&f);
			return 0;
		}

		if (neo_file_type == NEO_FILE_FIX)
		{
			memset(loadbuf, 0, partsz);
			if (partszf) FileReadAdv(&f, loadbuf, partszf);
			fix_convert(loadbuf, (uint8_t*)base, partsz);
		}
		else if (neo_file_type == NEO_FILE_SPR)
		{
			memset(loadbuf, 0, partsz);
			if (partszf) FileReadAdv(&f, loadbuf, partszf);
			spr_convert_dbl((uint16_t*)loadbuf, (uint16_t*)base, partsz / 2);
		}
		else
		{
			memset(base, ((index>=16) && (index<64)) ? 8 : 0, partsz);
			if (partszf) FileReadAdv(&f, base, partszf);
		}

		int new_progress = PROGRESS_MAX - ((((uint64_t)(remain - partsz))*PROGRESS_MAX) / size);
		if (progress != new_progress)
		{
			progress = new_progress;
			neogeo_osd_progress(name, progress);
		}
		munmap(base, partsz);
		remain -= partsz;
		map_addr += partsz;
	}

	close(memfd);
	FileClose(&f);

	return size;
}

static void notify_core(uint8_t index, uint32_t size)
{
	user_io_set_index(10);

	EnableFpga();
	spi8(UIO_FILE_TX);
	spi8(0xff);
	DisableFpga();

	char memcp = !(index == 9 || (index >= 16 && index < 64));
	printf("notify_core(%d,%d): memcp = %d\n", index, size, memcp);

	EnableFpga();
	spi8(UIO_FILE_TX_DAT);
	spi_w(index);
	spi_w((uint16_t)size);
	spi_w(size >> 16);
	spi_w(memcp); //copy flag
	spi_w(0);
	DisableFpga();

	EnableFpga();
	spi8(UIO_FILE_TX);
	spi8(0x00);
	DisableFpga();
}

static uint32_t crom_sz = 0;
static uint32_t neogeo_tx(const char* path, const char* name, uint8_t neo_file_type, int8_t index, uint32_t offset, uint32_t size, uint32_t expand = 0)
{
	/*
	if (index >= 0) neogeo_file_tx(path, name, neo_file_type, index, offset, size);
	return 0;
	*/

	uint32_t sz = 0;

	if (index >= 64)
	{
		sz = load_crom_to_mem(path, name, index, offset, size);
		if (sz > crom_sz) crom_sz = sz;
		return sz;
	}

	if (crom_sz)
	{
		sz = crom_sz;
		notify_core(15, crom_sz);
		crom_sz = 0;
	}

	if (index >= 0)
	{
		sz = load_rom_to_mem(path, name, neo_file_type, index, offset, size, expand);
		if (sz) notify_core(index, sz);
	}

	return sz;
}

struct rom_info
{
	char name[256];
	char altname[256];
};

static rom_info roms[1000];
static uint32_t rom_cnt = 0;

static int xml_scan(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	(void)(sd);

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!strcasecmp(node->tag, "romset") && (rom_cnt < (sizeof(roms) / sizeof(roms[0]))))
		{
			memset(&roms[rom_cnt], 0, sizeof(rom_info));
			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "name"))
				{
					if (strchr(node->attributes[i].value, ','))
						snprintf(roms[rom_cnt].name, sizeof(roms[rom_cnt].name) - 1, ",%s,", node->attributes[i].value);
					else
						strncpy(roms[rom_cnt].name, node->attributes[i].value, sizeof(roms[rom_cnt].name) - 1);
					strcpy(roms[rom_cnt].altname, "No name");
				}
			}

			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "altname"))
				{
					memset(roms[rom_cnt].altname, 0, sizeof(roms[rom_cnt].altname));
					strncpy(roms[rom_cnt].altname, node->attributes[i].value, sizeof(roms[rom_cnt].altname) - 1);
				}
			}
			rom_cnt++;
		}
		break;

	case XML_EVENT_END_NODE:
		break;

	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		break;
	default:
		break;
	}

	return true;
}

static int xml_get_altname(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	char* altname = (char*)sd->user;

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!strcasecmp(node->tag, "romset"))
		{
			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "name")) strncpy(altname, node->attributes[i].value, 255);
			}

			for (int i = 0; i < node->n_attributes; i++)
			{
				if (!strcasecmp(node->attributes[i].name, "altname")) strncpy(altname, node->attributes[i].value, 255);
			}
		}
		break;

	case XML_EVENT_END_NODE:
		break;

	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		break;
	default:
		break;
	}

	return true;
}

int neogeo_scan_xml()
{
	static char full_path[1024];
	sprintf(full_path, "%s/%s/romsets.xml", getRootDir(), HomeDir);
	SAX_Callbacks sax;
	SAX_Callbacks_init(&sax);

	memset(roms, 0, sizeof(roms));
	rom_cnt = 0;
	sax.all_event = xml_scan;
	XMLDoc_parse_file_SAX(full_path, &sax, 0);
	return rom_cnt;
}

char *neogeo_get_altname(char *path, char *name)
{
	static char full_path[1024];
	strcpy(full_path, path);
	strcat(full_path, "/");
	strcat(full_path, name);
	strcat(full_path, "/romset.xml");

	if (!access(full_path, F_OK))
	{
		static char altname[256];
		altname[0] = 0;

		SAX_Callbacks sax;
		SAX_Callbacks_init(&sax);

		sax.all_event = xml_get_altname;
		XMLDoc_parse_file_SAX(full_path, &sax, &altname);
		if (*altname) return altname;
	}

	sprintf(full_path, ",%s,", name);
	for (uint32_t i = 0; i < rom_cnt; i++)
	{
		if (roms[i].name[0] == ',')
		{
			char *p = strcasestr(roms[i].name, full_path);
			if (p)
			{
				if(p == roms[i].name) return roms[i].altname;

				sprintf(full_path, "%s (%s)", roms[i].altname, name);
				return full_path;
			}
		}
		else if (!strcasecmp(name, roms[i].name))
		{
			return roms[i].altname;
		}
	}
	return NULL;
}

static int has_name(const char *nameset, const char *name)
{
	if (strchr(nameset, ','))
	{
		static char set[256], nm[32];
		snprintf(set, sizeof(set) - 1, ",%s,", nameset);
		snprintf(nm, sizeof(nm) - 1, ",%s,", name);

		return strcasestr(set, nm) ? 1 : 0;
	}

	return !strcasecmp(nameset, name);
}

static int checked_ok;
static int romsets = 0;
static int xml_check_files(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	static int in_correct_romset = 0;
	static char full_path[1024];

	const char* path = (const char*)sd->user;
	if (!path) return 0;
	const char* romset = strrchr(path, '/');
	if (!romset) return 0;
	romset++;

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!strcasecmp(node->tag, "romsets")) romsets = 1;

		if (!strcasecmp(node->tag, "romset"))
		{
			if (!romsets)
			{
				in_correct_romset = 1;
			}
			else if (has_name(node->attributes[0].value, romset))
			{
				printf("Romset %s found !\n", romset);
				in_correct_romset = 1;
			}
			else
			{
				in_correct_romset = 0;
			}
		}
		if (in_correct_romset)
		{
			if (!strcasecmp(node->tag, "file"))
			{
				for (int i = 0; i < node->n_attributes; i++)
				{
					if (!strcasecmp(node->attributes[i].name, "name"))
					{
						sprintf(full_path, "%s/%s", path, node->attributes[i].value);
						if (FileExists(full_path))
						{
							printf("Found %s\n", full_path);
							break;
						}
						else
						{
							printf("Missing %s\n", full_path);
							sprintf(full_path, "Missing %s !", node->attributes[i].value);
							Info(full_path);
							return false;
						}
					}
				}
			}
		}
		break;

	case XML_EVENT_END_NODE:
		if (in_correct_romset)
		{
			if (!strcasecmp(node->tag, "romset"))
			{
				checked_ok = true;
				return false;
			}
		}
		if (!strcasecmp(node->tag, "romsets"))
		{
			printf("Couldn't find romset %s\n", romset);
			return false;
		}
		break;

	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		break;
	default:
		break;
	}

	return true;
}

#define VROM_SIZE  (16 * 1024 * 1024)

static int xml_load_files(XMLEvent evt, const XMLNode* node, SXML_CHAR* text, const int n, SAX_Data* sd)
{
	static char file_name[16 + 1] { "" };
	static int in_correct_romset = 0;
	static int in_file = 0;
	static unsigned char file_index = 0;
	static char file_type = 0;
	static unsigned long int file_offset = 0, file_size = 0, vromb_offset = 0;
	static unsigned char hw_type = 0, use_pcm = 0;
	static int file_cnt = 0;
	static int vrom_mirror = 1;

	const char* path = (const char*)sd->user;
	if (!path) return 0;
	const char* romset = strrchr(path, '/');
	if (!romset) return 0;
	romset++;

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!strcasecmp(node->tag, "romset")) {
			file_cnt = 0;
			vromb_offset = 0;
			vrom_mirror = 1;
			use_pcm = 1;
			hw_type = 0;
			if (!romsets) in_correct_romset = 1;
			for (int i = 0; i < node->n_attributes; i++) {
				if (romsets && !strcasecmp(node->attributes[i].name, "name")) {
					if (has_name(node->attributes[i].value, romset)) {
						printf("Romset %s found !\n", romset);
						in_correct_romset = 1;
					} else {
						in_correct_romset = 0;
					}
				} else if (!strcasecmp(node->attributes[i].name, "hw")) {
					hw_type = atoi(node->attributes[i].value);
				} else if (!strcasecmp(node->attributes[i].name, "pcm")) {
					use_pcm = atoi(node->attributes[i].value);
				} else if (!strcasecmp(node->attributes[i].name, "vromb_offset")) {
					vromb_offset = strtoul(node->attributes[i].value, NULL, 0);
					use_pcm = 0;
				} else if (!strcasecmp(node->attributes[i].name, "vrom_mirror")) {
					vrom_mirror = strtoul(node->attributes[i].value, NULL, 0);
				}
			}
		}
		if (in_correct_romset) {
			if (!strcasecmp(node->tag, "file")) {
				file_offset = 0;
				file_size = 0;
				file_type = NEO_FILE_RAW;
				file_index = 0;

				int use_index = 0;
				for (int i = 0; i < node->n_attributes; i++) {
					if (!strcasecmp(node->attributes[i].name, "index")) use_index = 1;
				}

				//printf("using index = %d\n", use_index);

				for (int i = 0; i < node->n_attributes; i++) {
					if (!strcasecmp(node->attributes[i].name, "name"))
						strncpy(file_name, node->attributes[i].value, 16);

					if (use_index) {
						if (!strcasecmp(node->attributes[i].name, "index"))
						{
							file_index = atoi(node->attributes[i].value);
							if (file_index >= 64 || file_index == 15) file_type = NEO_FILE_SPR;
							else if (file_index == 2 || file_index == 8) file_type = NEO_FILE_FIX;
						}
					}
					else
					{
						if (!strcasecmp(node->attributes[i].name, "type")) {
							switch (*node->attributes[i].value)
							{
							case 'C':
								file_index = 15;
								file_type = NEO_FILE_SPR;
								break;

							case 'M':
								file_index = 9;
								break;

							case 'P':
								file_index = 4;
								break;

							case 'S':
								file_index = 8;
								file_type = NEO_FILE_FIX;
								break;

							case 'V':
								file_index = 16;
								break;
							}
						}
					}

					if (!strcasecmp(node->attributes[i].name, "offset"))
						file_offset = strtol(node->attributes[i].value, NULL, 0);

					if (!strcasecmp(node->attributes[i].name, "size"))
						file_size = strtol(node->attributes[i].value, NULL, 0);
				}
				in_file = 1;
				file_cnt++;
			}
		}
		break;

	case XML_EVENT_END_NODE:
		if (in_correct_romset) {
			if (!strcasecmp(node->tag, "romset"))
			{
				if (!file_cnt)
				{
					printf("No parts specified. Trying to load known files:\n");
					neogeo_tx(path, "prom", NEO_FILE_RAW, 4, 0, 0);
					neogeo_tx(path, "p1rom", NEO_FILE_RAW, 4, 0, 0);
					neogeo_tx(path, "p2rom", NEO_FILE_RAW, 6, 0, 0);
					neogeo_tx(path, "srom", NEO_FILE_FIX, 8, 0, 0);
					neogeo_tx(path, "crom0", NEO_FILE_SPR, 15, 0, 0);
					neogeo_tx(path, "m1rom", NEO_FILE_RAW, 9, 0, 0);
					if (vromb_offset)
					{
						neogeo_tx(path, "vroma0", NEO_FILE_RAW, 16, 0, vromb_offset, vrom_mirror ? 0 : VROM_SIZE);
						neogeo_tx(path, "vroma0", NEO_FILE_RAW, 48, vromb_offset, 0, vrom_mirror ? 0 : VROM_SIZE);
					}
					else
					{
						neogeo_tx(path, "vroma0", NEO_FILE_RAW, 16, 0, 0, vrom_mirror ? 0 : VROM_SIZE);
						if(!use_pcm) neogeo_tx(path, "vromb0", NEO_FILE_RAW, 48, 0, 0, vrom_mirror ? 0 : VROM_SIZE);
					}
				}

				printf("Setting cart hardware type to %u\n", hw_type);
				user_io_8bit_set_status(((uint32_t)hw_type & 3) << 24, 0x03000000);
				printf("Setting cart to%s use the PCM chip\n", use_pcm ? "" : " not");
				user_io_8bit_set_status(((uint32_t)use_pcm & 1) << 26, 0x04000000);
				return 0;
			}
			else if (!strcasecmp(node->tag, "file"))
			{
				if (in_file)
				{
					uint32_t expand = 0;
					if (!vrom_mirror && file_index >= 16 && file_index < 64)
					{
						expand = VROM_SIZE - (((file_index - 16) * 0x80000) & 0xFFFFFF);
					}

					neogeo_tx(path, file_name, file_type, file_index, file_offset, file_size, expand);
				}
				in_file = 0;
			}
		}
		break;

	case XML_EVENT_ERROR:
		printf("XML parse: %s: ERROR %d\n", text, n);
		break;
	default:
		break;
	}

	return true;
}

int neogeo_romset_tx(char* name)
{
	char *romset = strrchr(name, '/');
	if (!romset) return 0;
	romset++;

	int system_type;
	static char full_path[1024];

	system_type = (user_io_8bit_set_status(0, 0) >> 1) & 3;
	printf("System type: %u\n", system_type);

	user_io_8bit_set_status(1, 1);	// Maintain reset

	crom_sz = 0;

	// Look for the romset's file list in romsets.xml
	if (!(system_type & 2))
	{
		sprintf(full_path, "%s/%s/romset.xml", getRootDir(), name);
		if (access(full_path, F_OK)) sprintf(full_path, "%s/%s/romsets.xml", getRootDir(), HomeDir);

		printf("xml for %s: %s\n", name, full_path);

		SAX_Callbacks sax;
		SAX_Callbacks_init(&sax);

		checked_ok = false;
		romsets = 0;
		sax.all_event = xml_check_files;
		XMLDoc_parse_file_SAX(full_path, &sax, name);
		if (!checked_ok) return 0;

		sax.all_event = xml_load_files;
		XMLDoc_parse_file_SAX(full_path, &sax, name);
	}

	// Load system ROMs
	if (strcmp(romset, "debug")) {
		// Not loading the special 'debug' romset
		if (!(system_type & 2)) {
			sprintf(full_path, "%s/uni-bios.rom", HomeDir);
			if (FileExists(full_path)) {
				// Autoload Unibios for cart systems if present
				neogeo_tx(HomeDir, "uni-bios.rom", NEO_FILE_RAW, 0, 0, 0x20000);
			} else {
				// Otherwise load normal system roms
				if (system_type == 0)
					neogeo_tx(HomeDir, "neo-epo.sp1", NEO_FILE_RAW, 0, 0, 0x20000);
				else
					neogeo_tx(HomeDir, "sp-s2.sp1", NEO_FILE_RAW, 0, 0, 0x20000);
			}
		} else if (system_type == 2) {
			// NeoGeo CD
			neogeo_tx(HomeDir, "top-sp1.bin", NEO_FILE_RAW, 0, 0, 0x80000);
		} else {
			// NeoGeo CDZ
			neogeo_tx(HomeDir, "neocd.bin", NEO_FILE_RAW, 0, 0, 0x80000);
		}
	}

	//flush CROM if any.
	neogeo_tx(NULL, NULL, 0, -1, 0, 0);

	if (!(system_type & 2))	neogeo_tx(HomeDir, "sfix.sfix", NEO_FILE_FIX, 2, 0, 0x10000);
	neogeo_file_tx(HomeDir, "000-lo.lo", NEO_FILE_8BIT, 1, 0, 0x10000);

	if (!strcmp(romset, "kof95"))
	{
		printf("Enabled sprite gfx gap hack for kof95\n");
		user_io_8bit_set_status(0x10000000, 0x30000000);
	}
	else if (!strcmp(romset, "whp"))
	{
		printf("Enabled sprite gfx gap hack for whp\n");
		user_io_8bit_set_status(0x20000000, 0x30000000);
	}
	else if (!strcmp(romset, "kizuna"))
	{
		printf("Enabled sprite gfx gap hack for kizuna\n");
		user_io_8bit_set_status(0x30000000, 0x30000000);
	}
	else
	{
		user_io_8bit_set_status(0x00000000, 0x30000000);
	}

	FileGenerateSavePath((system_type & 2) ? "ngcd" : name, (char*)full_path);
	user_io_file_mount((char*)full_path, 2, 1);

	user_io_8bit_set_status(0, 1);	// Release reset

	return 1;
}