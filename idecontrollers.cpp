/*
* UAE - The Un*x Amiga Emulator
*
* Other IDE controllers
*
* (c) 2015 Toni Wilen
*/

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"

#include "memory.h"
#include "newcpu.h"
#include "uae.h"
#include "gui.h"
#include "filesys.h"
#include "threaddep/thread.h"
#include "debug.h"
#include "ide.h"
#include "idecontrollers.h"
#include "zfile.h"
#include "custom.h"
#include "rommgr.h"
#include "cpuboard.h"
#include "scsi.h"
#include "ncr9x_scsi.h"
#include "autoconf.h"

#define DEBUG_IDE 0
#define DEBUG_IDE_GVP 0
#define DEBUG_IDE_ALF 0
#define DEBUG_IDE_APOLLO 0
#define DEBUG_IDE_MASOBOSHI 0

#define GVP_IDE 0 // GVP A3001
#define ALF_IDE 1
#define APOLLO_IDE (ALF_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)
#define MASOBOSHI_IDE (APOLLO_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)
#define ADIDE_IDE (MASOBOSHI_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)
#define MTEC_IDE (ADIDE_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)
#define PROTAR_IDE (MTEC_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)
#define TOTAL_IDE (PROTAR_IDE + MAX_DUPLICATE_EXPANSION_BOARDS)

#define ALF_ROM_OFFSET 0x0100
#define GVP_IDE_ROM_OFFSET 0x8000
#define APOLLO_ROM_OFFSET 0x8000
#define ADIDE_ROM_OFFSET 0x8000
#define MASOBOSHI_ROM_OFFSET 0x0080
#define MASOBOSHI_ROM_OFFSET_END 0xf000
#define MASOBOSHI_SCSI_OFFSET 0xf800
#define MASOBOSHI_SCSI_OFFSET_END 0xfc00

/* masoboshi:

IDE 

- FFCC = base address, data (0)
- FF81 = -004B
- FF41 = -008B
- FF01 = -01CB
- FEC1 = -010B
- FE81 = -014B
- FE41 = -018B select (6)
- FE01 = -01CB status (7)
- FE03 = command (7)

- FA00 = ESP, 2 byte register spacing
- F9CC = data

- F047 = -0F85 (-0FB9) interrupt request? (bit 3)
- F040 = -0F8C interrupt request? (bit 1) Write anything = clear irq?
- F000 = some status register

- F04C = DMA address (long)
- F04A = number of words
- F044 = ???
- F047 = bit 7 = start dma

*/

#define MAX_IDE_UNITS 10

static struct ide_board *gvp_ide_rom_board, *gvp_ide_controller_board;
static struct ide_board *alf_board[MAX_DUPLICATE_EXPANSION_BOARDS];
static struct ide_board *apollo_board[MAX_DUPLICATE_EXPANSION_BOARDS];
static struct ide_board *masoboshi_board[MAX_DUPLICATE_EXPANSION_BOARDS];
static struct ide_board *adide_board[MAX_DUPLICATE_EXPANSION_BOARDS];
static struct ide_board *mtec_board[MAX_DUPLICATE_EXPANSION_BOARDS];
static struct ide_board *protar_board[MAX_DUPLICATE_EXPANSION_BOARDS];

static struct ide_hdf *idecontroller_drive[TOTAL_IDE * 2];
static struct ide_thread_state idecontroller_its;

static struct ide_board *ide_boards[MAX_IDE_UNITS + 1];

static struct ide_board *allocide(struct ide_board **idep, struct romconfig *rc, int ch)
{
	struct ide_board *ide;

	if (ch < 0) {
		if (*idep) {
			remove_ide_unit(&(*idep)->ide, 0);
			xfree(*idep);
			*idep = NULL;
		}
		ide = xcalloc(struct ide_board, 1);
		for (int i = 0; i < MAX_IDE_UNITS; i++) {
			if (ide_boards[i] == NULL) {
				ide_boards[i] = ide;
				rc->unitdata = ide;
				ide->rc = rc;
				if (idep)
					*idep = ide;
				return ide;
			}
		}
	}
	return *idep;
}

static struct ide_board *getide(struct romconfig *rc)
{
	for (int i = 0; i < MAX_IDE_UNITS; i++) {
		if (ide_boards[i]) {
			struct ide_board *ide = ide_boards[i];
			if (ide->rc == rc) {
				ide->rc = NULL;
				return ide;
			}
		}
	}
	return NULL;
}

static struct ide_board *getideboard(uaecptr addr)
{
	for (int i = 0; ide_boards[i]; i++) {
		if (!ide_boards[i]->baseaddress && !ide_boards[i]->configured)
			return ide_boards[i];
		if ((addr & ~ide_boards[i]->mask) == ide_boards[i]->baseaddress)
			return ide_boards[i];
	}
	return NULL;
}

static void init_ide(struct ide_board *board, int ide_num, bool byteswap, bool adide)
{
	struct ide_hdf **idetable = &idecontroller_drive[ide_num * 2];
	alloc_ide_mem (idetable, 2, &idecontroller_its);
	board->ide = idetable[0];
	idetable[0]->board = board;
	idetable[1]->board = board;
	idetable[0]->byteswap = byteswap;
	idetable[1]->byteswap = byteswap;
	idetable[0]->adide = adide;
	idetable[1]->adide = adide;
	ide_initialize(idecontroller_drive, ide_num);
	idecontroller_its.idetable = idecontroller_drive;
	idecontroller_its.idetotal = TOTAL_IDE * 2;
	start_ide_thread(&idecontroller_its);
}

static void add_ide_standard_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc, struct ide_board **ideboard, int idetype, bool byteswap, bool adide)
{
	struct ide_hdf *ide;
	struct ide_board *ideb;
	ideb = allocide(&ideboard[ci->controller_type_unit], rc, ch);
	if (!ideb)
		return;
	ideb->keepautoconfig = true;
	ideb->type = idetype;
	ide = add_ide_unit (&idecontroller_drive[(idetype + ci->controller_type_unit) * 2], 2, ch, ci, rc);
	init_ide(ideb, idetype + ci->controller_type_unit, byteswap, adide);
}

static bool ide_interrupt_check(struct ide_board *board)
{
	if (!board->configured)
		return false;
	bool irq = ide_irq_check(board->ide);
#if 0
	if (board->irq != irq)
		write_log(_T("IDE irq %d -> %d\n"), board->irq, irq);
#endif
	board->irq = irq;
	return irq;
}

static bool ide_rethink(struct ide_board *board)
{
	bool irq = false;
	if (board->configured) {
		if (board->intena && ide_interrupt_check(board)) {
			irq = true;
		}
	}
	return irq;
}

void idecontroller_rethink(void)
{
	bool irq = false;
	for (int i = 0; ide_boards[i]; i++) {
		irq |= ide_rethink(ide_boards[i]);
	}
	if (irq && !(intreq & 0x0008)) {
		INTREQ_0(0x8000 | 0x0008);
	}
}

void idecontroller_hsync(void)
{
	for (int i = 0; ide_boards[i]; i++) {
		struct ide_board *board = ide_boards[i];
		if (board->configured) {
			ide_interrupt_hsync(board->ide);
			if (ide_interrupt_check(board)) {
				idecontroller_rethink();
			}
		}
	}
}

void idecontroller_free_units(void)
{
	for (int i = 0; i < TOTAL_IDE * 2; i++) {
		remove_ide_unit(idecontroller_drive, i);
	}
}

static void reset_ide(struct ide_board *board)
{
	board->configured = 0;
	board->intena = false;
	board->enabled = false;
}

void idecontroller_reset(void)
{
	for (int i = 0; ide_boards[i]; i++) {
		reset_ide(ide_boards[i]);
	}
}

void idecontroller_free(void)
{
	stop_ide_thread(&idecontroller_its);
}

static bool is_gvp2_intreq(uaecptr addr)
{
	if (ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SII) && (addr & 0x440) == 0x440)
		return true;
	return false;
}
static bool is_gvp1_intreq(uaecptr addr)
{
	if (ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SI) && (addr & 0x440) == 0x40)
		return true;
	return false;
}

static uae_u32 get_ide_reg(struct ide_board *board, int reg)
{
	struct ide_hdf *ide = board->ide;
	if (ide->ide_drv)
		ide = ide->pair;
	if (reg == 0)
		return ide_get_data(ide);
	else
		return ide_read_reg(ide, reg);
}
static void put_ide_reg(struct ide_board *board, int reg, uae_u32 v)
{
	struct ide_hdf *ide = board->ide;
	if (ide->ide_drv)
		ide = ide->pair;
	if (reg == 0)
		ide_put_data(ide, v);
	else
		ide_write_reg(ide, reg, v);
}

static int get_gvp_reg(uaecptr addr, struct ide_board *board)
{
	int reg = -1;

	if (addr & 0x1000) {
		reg = IDE_SECONDARY + ((addr >> 8) & 7);
	} else if (addr & 0x0800) {
		reg = (addr >> 8) & 7;
	}
	if (!(addr & 0x400) && (addr & 0x20)) {
		if (reg < 0)
			reg = 0;
		int extra = (addr >> 1) & 15;
		if (extra >= 8)
			reg |= IDE_SECONDARY;
		reg |= extra;
	}
	if (reg >= 0)
		reg &= IDE_SECONDARY | 7;

	return reg;
}

static int get_apollo_reg(uaecptr addr, struct ide_board *board)
{
	if (addr & 0x4000)
		return -1;
	int reg = addr & 0x1fff;
	reg >>= 10;
	if (addr & 0x2000)
		reg |= IDE_SECONDARY;
	if (reg != 0 && !(addr & 1))
		reg = -1;
	write_log(_T("APOLLO %04x = %d\n"), addr, reg);
	return reg;
}

static int get_alf_reg(uaecptr addr, struct ide_board *board)
{
	if (addr & 0x8000)
		return -1;
	if (addr & 0x4000) {
		;
	} else if (addr & 0x1000) {
		addr &= 0xfff;
		addr >>= 9;
	} else if (addr & 0x2000) {
		addr &= 0xfff;
		addr >>= 9;
		addr |= IDE_SECONDARY;
	}
	return addr;
}

static int get_masoboshi_reg(uaecptr addr, struct ide_board *board)
{
	int reg;
	if (addr < 0xfc00)
		return -1;
	reg = 7 - ((addr >> 6) & 7);
	if (addr < 0xfe00)
		reg |= IDE_SECONDARY;
	return reg;
}

static int get_adide_reg(uaecptr addr, struct ide_board *board)
{
	int reg;
	if (addr & 0x8000)
		return -1;
	reg = (addr >> 1) & 7;
	if (addr & 0x10)
		reg |= IDE_SECONDARY;
	return reg;
}

static int getidenum(struct ide_board *board, struct ide_board **arr)
{
	for (int i = 0; i < MAX_DUPLICATE_EXPANSION_BOARDS; i++) {
		if (board == arr[i])
			return i;
	}
	return 0;
}

static uae_u32 ide_read_byte(struct ide_board *board, uaecptr addr)
{
	uaecptr oaddr = addr;
	uae_u8 v = 0xff;

#ifdef JIT
	special_mem |= S_READ;
#endif

	addr &= board->mask;

#if DEBUG_IDE
	write_log(_T("IDE IO BYTE READ %08x %08x\n"), addr, M68K_GETPC);
#endif
	
	if (addr < 0x40 && (!board->configured || board->keepautoconfig))
		return board->acmemory[addr];

	if (board->type == ALF_IDE) {

		if (addr < 0x1100 || (addr & 1)) {
			if (board->rom)
				v = board->rom[addr & board->rom_mask];
			return v;
		}
		int regnum = get_alf_reg(addr, board);
		if (regnum >= 0) {
			v = get_ide_reg(board, regnum);
		}
#if DEBUG_IDE_ALF
		write_log(_T("ALF GET %08x %02x %d %08x\n"), addr, v, regnum, M68K_GETPC);
#endif

	} else if (board->type == MASOBOSHI_IDE) {
		int regnum = -1;
		bool rom = false;
		if (addr >= MASOBOSHI_ROM_OFFSET && addr < MASOBOSHI_ROM_OFFSET_END) {
			if (board->rom) {
				v = board->rom[addr & board->rom_mask];
				rom = true;
			}
		} else if (addr >= 0xf000 && addr <= 0xf007) {
			if (board->subtype)
				v = masoboshi_ncr9x_scsi_get(oaddr, getidenum(board, masoboshi_board));
		} else if (addr == 0xf040) {
			v = 1;
			if (ide_irq_check(board->ide)) {
				v |= 2;
				board->irq = true;
			}
			if (board->irq) {
				v &= ~1;
			}
			v |= masoboshi_ncr9x_scsi_get(oaddr, getidenum(board, masoboshi_board));
		} else if (addr == 0xf047) {
			v = board->state;
		} else {
			regnum = get_masoboshi_reg(addr, board);
			if (regnum >= 0) {
				v = get_ide_reg(board, regnum);
			} else if (addr >= MASOBOSHI_SCSI_OFFSET && addr < MASOBOSHI_SCSI_OFFSET_END) {
				if (board->subtype)
					v = masoboshi_ncr9x_scsi_get(oaddr, getidenum(board, masoboshi_board));
				else
					v = 0xff;
			}
		}
#if DEBUG_IDE_MASOBOSHI
		if (!rom)
			write_log(_T("MASOBOSHI BYTE GET %08x %02x %d %08x\n"), addr, v, regnum, M68K_GETPC);
#endif
	} else if (board->type == APOLLO_IDE) {

		if (addr >= APOLLO_ROM_OFFSET) {
			if (board->rom)
				v = board->rom[(addr - APOLLO_ROM_OFFSET) & board->rom_mask];
		} else if (board->configured) {
			if ((addr & 0xc000) == 0x4000) {
				v = apollo_scsi_bget(oaddr);
			} else if (addr < 0x4000) {
				int regnum = get_apollo_reg(addr, board);
				if (regnum >= 0) {
					v = get_ide_reg(board, regnum);
				} else {
					v = 0;
				}
			}
		}

	} else if (board->type == GVP_IDE) {

		if (addr >= GVP_IDE_ROM_OFFSET) {
			if (board->rom) {
				if (addr & 1)
					v = 0xe8; // board id
				else 
					v = board->rom[((addr - GVP_IDE_ROM_OFFSET) / 2) & board->rom_mask];
				return v;
			}
			v = 0xe8;
#if DEBUG_IDE_GVP
			write_log(_T("GVP BOOT GET %08x %02x %08x\n"), addr, v, M68K_GETPC);
#endif
			return v;
		}
		if (board->configured) {
			if (board == gvp_ide_rom_board && ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SII)) {
				if (addr == 0x42) {
					v = 0xff;
				}
#if DEBUG_IDE_GVP
				write_log(_T("GVP BOOT GET %08x %02x %08x\n"), addr, v, M68K_GETPC);
#endif
			} else {
				int regnum = get_gvp_reg(addr, board);
#if DEBUG_IDE_GVP
				write_log(_T("GVP IDE GET %08x %02x %d %08x\n"), addr, v, regnum, M68K_GETPC);
#endif
				if (regnum >= 0) {
					v = get_ide_reg(board, regnum);
				} else if (is_gvp2_intreq(addr)) {
					v = board->irq ? 0x40 : 0x00;
#if DEBUG_IDE_GVP
					write_log(_T("GVP IRQ %02x\n"), v);
#endif
					ide_interrupt_check(board);
				} else if (is_gvp1_intreq(addr)) {
					v = board->irq ? 0x80 : 0x00;
#if DEBUG_IDE_GVP
					write_log(_T("GVP IRQ %02x\n"), v);
#endif
					ide_interrupt_check(board);
				}
			}
		} else {
			v = 0xff;
		}

	} else if (board->type == ADIDE_IDE) {

		if (addr & ADIDE_ROM_OFFSET) {
			v = board->rom[addr & board->rom_mask];
		} else if (board->configured) {
			int regnum = get_adide_reg(addr, board);
			v = get_ide_reg(board, regnum);
			v = adide_decode_word(v);
		}

	} else if (board->type == MTEC_IDE) {

		if (!(addr & 0x8000)) {
			v = board->rom[addr & board->rom_mask];
		} else if (board->configured) {
			v = get_ide_reg(board, (addr >> 8) & 7);
		}

	} else if (board->type == PROTAR_IDE) {

		v = board->rom[addr & board->rom_mask];

	}
	return v;
}

static uae_u32 ide_read_word(struct ide_board *board, uaecptr addr)
{
	uae_u32 v = 0xffff;

#ifdef JIT
	special_mem |= S_READ;
#endif

	addr &= board->mask;

	if (addr < 0x40 && (!board->configured || board->keepautoconfig)) {
		v = board->acmemory[addr] << 8;
		v |= board->acmemory[addr + 1];
		return v;
	}

	if (board->type == APOLLO_IDE) {

		if (addr >= APOLLO_ROM_OFFSET) {
			if (board->rom) {
				v = board->rom[(addr + 0 - APOLLO_ROM_OFFSET) & board->rom_mask];
				v <<= 8;
				v |= board->rom[(addr + 1 - APOLLO_ROM_OFFSET) & board->rom_mask];
			}
		}
	}


	if (board->configured) {

		if (board->type == ALF_IDE) {

			int regnum = get_alf_reg(addr, board);
			if (regnum == IDE_DATA) {
				v = get_ide_reg(board, IDE_DATA);
			} else {
				v = 0;
				if (addr == 0x4000 && board->intena)
					v = board->irq ? 0x8000 : 0x0000;
#if DEBUG_IDE_ALF
				write_log(_T("ALF IO WORD READ %08x %08x\n"), addr, M68K_GETPC);
#endif
			}

		} else if (board->type == MASOBOSHI_IDE) {

			if (addr >= MASOBOSHI_ROM_OFFSET && addr < MASOBOSHI_ROM_OFFSET_END) {
				if (board->rom) {
					v = board->rom[addr & board->rom_mask] << 8;
					v |= board->rom[(addr + 1) & board->rom_mask];
				}
			} else {
				int regnum = get_masoboshi_reg(addr, board);
				if (regnum == IDE_DATA) {
					v = get_ide_reg(board, IDE_DATA);
				} else {
					v = ide_read_byte(board, addr) << 8;
					v |= ide_read_byte(board, addr + 1);
				}
			}

		} else if (board->type == APOLLO_IDE) {

			if ((addr & 0xc000) == 0x4000) {
				v = apollo_scsi_bget(addr);
				v <<= 8;
				v |= apollo_scsi_bget(addr + 1);
			} else if (addr < 0x4000) {
				int regnum = get_apollo_reg(addr, board);
				if (regnum == IDE_DATA) {
					v = get_ide_reg(board, IDE_DATA);
				} else {
					v = 0;
				}
			}

		} else if (board->type == GVP_IDE) {

			if (board == gvp_ide_controller_board || ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SI)) {
				if (addr < 0x60) {
					if (is_gvp1_intreq(addr))
						v = gvp_ide_controller_board->irq ? 0x8000 : 0x0000;
					else if (addr == 0x40) {
						if (ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SII))
							v = board->intena ? 8 : 0;
					}
#if DEBUG_IDE_GVP
					write_log(_T("GVP IO WORD READ %08x %08x\n"), addr, M68K_GETPC);
#endif
				} else {
					int regnum = get_gvp_reg(addr, board);
					if (regnum == IDE_DATA) {
						v = get_ide_reg(board, IDE_DATA);
#if DEBUG_IDE_GVP > 2
						write_log(_T("IDE WORD READ %04x\n"), v);
#endif
					} else {
						v = ide_read_byte(board, addr) << 8;
						v |= ide_read_byte(board, addr + 1);
					}
				}	
			}

		} else if (board->type == ADIDE_IDE) {

			int regnum = get_adide_reg(addr, board);
			if (regnum == IDE_DATA) {
				v = get_ide_reg(board, IDE_DATA);
			} else {
				v = get_ide_reg(board, regnum) << 8;
				v = adide_decode_word(v);
			}

		} else if (board->type == MTEC_IDE) {

			if (board->configured && (addr & 0x8000)) {
				int regnum = (addr >> 8) & 7;
				if (regnum == IDE_DATA)
					v = get_ide_reg(board, regnum);
				else
					v = ide_read_byte(board, addr) << 8;
			}

		}
	}

#if DEBUG_IDE
	write_log(_T("IDE IO WORD READ %08x %04x %08x\n"), addr, v, M68K_GETPC);
#endif

	return v;
}

static void ide_write_byte(struct ide_board *board, uaecptr addr, uae_u8 v)
{
	uaecptr oaddr = addr;
	addr &= board->mask;

#ifdef JIT
	special_mem |= S_WRITE;
#endif

#if DEBUG_IDE
	write_log(_T("IDE IO BYTE WRITE %08x=%02x %08x\n"), addr, v, M68K_GETPC);
#endif

	if (!board->configured) {
		addrbank *ab = board->bank;
		if (addr == 0x48) {
			map_banks_z2(ab, v, (board->mask + 1) >> 16);
			board->baseaddress = v << 16;
			board->configured = 1;
			expamem_next(ab, NULL);
			return;
		}
		if (addr == 0x4c) {
			board->configured = 1;
			expamem_shutup(ab);
			return;
		}
	}
	if (board->configured) {
		if (board->type == ALF_IDE) {
			int regnum = get_alf_reg(addr, board);
			if (regnum >= 0)
				put_ide_reg(board, regnum, v);
#if DEBUG_IDE_ALF
			write_log(_T("ALF PUT %08x %02x %d %08x\n"), addr, v, regnum, M68K_GETPC);
#endif
		} else if (board->type == MASOBOSHI_IDE) {

#if DEBUG_IDE_MASOBOSHI
			write_log(_T("MASOBOSHI IO BYTE PUT %08x %02x %08x\n"), addr, v, M68K_GETPC);
#endif
			int regnum = get_masoboshi_reg(addr, board);
			if (regnum >= 0) {
				put_ide_reg(board, regnum, v);
			} else if (addr >= MASOBOSHI_SCSI_OFFSET && addr < MASOBOSHI_SCSI_OFFSET_END) {
				if (board->subtype)
					masoboshi_ncr9x_scsi_put(oaddr, v, getidenum(board, masoboshi_board));
			} else if ((addr >= 0xf000 && addr <= 0xf007)) {
				if (board->subtype)
					masoboshi_ncr9x_scsi_put(oaddr, v, getidenum(board, masoboshi_board));
			} else if (addr >= 0xf04a && addr <= 0xf04f) {
				// dma controller
				masoboshi_ncr9x_scsi_put(oaddr, v, getidenum(board, masoboshi_board));
			} else if (addr >= 0xf040 && addr < 0xf048) {
				masoboshi_ncr9x_scsi_put(oaddr, v, getidenum(board, masoboshi_board));
				if (addr == 0xf047) {
					board->state = v;
					board->intena = (v & 8) != 0;
				}
				if (addr == 0xf040) {
					board->irq = false;
				}
				write_log(_T("MASOBOSHI STATUS BYTE PUT %08x %02x %08x\n"), addr, v, M68K_GETPC);
			}

		} else if (board->type == APOLLO_IDE) {

			if ((addr & 0xc000) == 0x4000) {
				apollo_scsi_bput(oaddr, v);
			} else if (addr < 0x4000) {
				int regnum = get_apollo_reg(addr, board);
				if (regnum >= 0) {
					put_ide_reg(board, regnum, v);
				}
			}

		} else if (board->type == GVP_IDE) {

			if (board == gvp_ide_rom_board && ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SII)) {
#if DEBUG_IDE_GVP
				write_log(_T("GVP BOOT PUT %08x %02x %08x\n"), addr, v, M68K_GETPC);
#endif
			} else {
				int regnum = get_gvp_reg(addr, board);
#if DEBUG_IDE_GVP
				write_log(_T("GVP IDE PUT %08x %02x %d %08x\n"), addr, v, regnum, M68K_GETPC);
#endif
				if (regnum >= 0)
					put_ide_reg(board, regnum, v);
			}

		} else if (board->type == ADIDE_IDE) {

			if (board->configured) {
				int regnum = get_adide_reg(addr, board);
				v = adide_encode_word(v);
				put_ide_reg(board, regnum, v);
			}

		} else if (board->type == MTEC_IDE) {

			if (board->configured && (addr & 0x8000)) {
				put_ide_reg(board, (addr >> 8) & 7, v);
			}

		}

	}
}

static void ide_write_word(struct ide_board *board, uaecptr addr, uae_u16 v)
{
	addr &= board->mask;

#ifdef JIT
	special_mem |= S_WRITE;
#endif

	if (addr == 0xf04a)
		addr &= 0xffff;

#if DEBUG_IDE
	write_log(_T("IDE IO WORD WRITE %08x=%04x %08x\n"), addr, v, M68K_GETPC);
#endif
	if (board->configured) {
		if (board->type == ALF_IDE) {

			int regnum = get_alf_reg(addr, board);
			if (regnum == IDE_DATA) {
				put_ide_reg(board, IDE_DATA, v);
			} else {
#if DEBUG_IDE_ALF
				write_log(_T("ALF IO WORD WRITE %08x %04x %08x\n"), addr, v, M68K_GETPC);
#endif
			}

		} else if (board->type == MASOBOSHI_IDE) {

			int regnum = get_masoboshi_reg(addr, board);
			if (regnum == IDE_DATA) {
				put_ide_reg(board, IDE_DATA, v);
			} else {
				ide_write_byte(board, addr, v >> 8);
				ide_write_byte(board, addr + 1, v);
			}
#if DEBUG_IDE_MASOBOSHI
			write_log(_T("MASOBOSHI IO WORD WRITE %08x %04x %08x\n"), addr, v, M68K_GETPC);
#endif
	
		} else if (board->type == APOLLO_IDE) {

			if ((addr & 0xc000) == 0x4000) {
				apollo_scsi_bput(addr, v >> 8);
				apollo_scsi_bput(addr + 1, v);
			} else if (addr < 0x4000) {
				int regnum = get_apollo_reg(addr, board);
				if (regnum == IDE_DATA) {
					put_ide_reg(board, IDE_DATA, v);
				}
			}

		} else if (board->type == GVP_IDE) {

			if (board == gvp_ide_controller_board || ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SI)) {
				if (addr < 0x60) {
#if DEBUG_IDE_GVP
					write_log(_T("GVP IO WORD WRITE %08x %04x %08x\n"), addr, v, M68K_GETPC);
#endif
					if (addr == 0x40 && ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SII))
						board->intena = (v & 8) != 0;
				} else {
					int regnum = get_gvp_reg(addr, board);
					if (regnum == IDE_DATA) {
						put_ide_reg(board, IDE_DATA, v);
#if DEBUG_IDE_GVP > 2
						write_log(_T("IDE WORD WRITE %04x\n"), v);
#endif
					} else {
						ide_write_byte(board, addr, v >> 8);
						ide_write_byte(board, addr + 1, v & 0xff);
					}
				}
			}

		} else if (board->type == ADIDE_IDE) {

			int regnum = get_adide_reg(addr, board);
			if (regnum == IDE_DATA) {
				put_ide_reg(board, IDE_DATA, v);
			} else {
				v = adide_encode_word(v);
				put_ide_reg(board, regnum, v >> 8);
			}

		} else if (board->type == MTEC_IDE) {

			if (board->configured && (addr & 0x8000)) {
				int regnum = (addr >> 8) & 7;
				if (regnum == IDE_DATA)
					put_ide_reg(board, regnum, v);
				else
					ide_write_byte(board, addr, v >> 8);
			}

		}
	}
}

IDE_MEMORY_FUNCTIONS(ide_controller_gvp, ide, gvp_ide_controller_board);

addrbank gvp_ide_controller_bank = {
	ide_controller_gvp_lget, ide_controller_gvp_wget, ide_controller_gvp_bget,
	ide_controller_gvp_lput, ide_controller_gvp_wput, ide_controller_gvp_bput,
	default_xlate, default_check, NULL, NULL, _T("GVP IDE"),
	dummy_lgeti, dummy_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

IDE_MEMORY_FUNCTIONS(ide_rom_gvp, ide, gvp_ide_rom_board);

addrbank gvp_ide_rom_bank = {
	ide_rom_gvp_lget, ide_rom_gvp_wget, ide_rom_gvp_bget,
	ide_rom_gvp_lput, ide_rom_gvp_wput, ide_rom_gvp_bput,
	default_xlate, default_check, NULL, NULL, _T("GVP BOOT"),
	dummy_lgeti, dummy_wgeti, ABFLAG_IO | ABFLAG_SAFE
};

static void REGPARAM2 ide_generic_bput (uaecptr addr, uae_u32 b)
{
	struct ide_board *ide = getideboard(addr);
	if (ide)
		ide_write_byte(ide, addr, b);
}
static void REGPARAM2 ide_generic_wput (uaecptr addr, uae_u32 b)
{
	struct ide_board *ide = getideboard(addr);
	if (ide)
		ide_write_word(ide, addr, b);
}
static void REGPARAM2 ide_generic_lput (uaecptr addr, uae_u32 b)
{
	struct ide_board *ide = getideboard(addr);
	if (ide) {
		ide_write_word(ide, addr, b >> 16);
		ide_write_word(ide, addr + 2, b);
	}
}
static uae_u32 REGPARAM2 ide_generic_bget (uaecptr addr)
{
	struct ide_board *ide = getideboard(addr);
	if (ide)
		return ide_read_byte(ide, addr);
	return 0;
}
static uae_u32 REGPARAM2 ide_generic_wget (uaecptr addr)
{
	struct ide_board *ide = getideboard(addr);
	if (ide)
		return ide_read_word(ide, addr);
	return 0;
}
static uae_u32 REGPARAM2 ide_generic_lget (uaecptr addr)
{
	struct ide_board *ide = getideboard(addr);
	if (ide) {
		uae_u32 v = ide_read_word(ide, addr) << 16;
		v |= ide_read_word(ide, addr + 2);
		return v;
	}
	return 0;
}
static addrbank ide_bank_generic = {
	ide_generic_lget, ide_generic_wget, ide_generic_bget,
	ide_generic_lput, ide_generic_wput, ide_generic_bput,
	default_xlate, default_check, NULL, NULL, _T("IDE"),
	dummy_lgeti, dummy_wgeti, ABFLAG_IO | ABFLAG_SAFE
};


static void ew(struct ide_board *ide, int addr, uae_u32 value)
{
	addr &= 0xffff;
	if (addr == 00 || addr == 02 || addr == 0x40 || addr == 0x42) {
		ide->acmemory[addr] = (value & 0xf0);
		ide->acmemory[addr + 2] = (value & 0x0f) << 4;
	} else {
		ide->acmemory[addr] = ~(value & 0xf0);
		ide->acmemory[addr + 2] = ~((value & 0x0f) << 4);
	}
}

static const uae_u8 gvp_ide2_rom_autoconfig[16] = { 0xd1, 0x0d, 0x00, 0x00, 0x07, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 };
static const uae_u8 gvp_ide2_controller_autoconfig[16] = { 0xc1, 0x0b, 0x00, 0x00, 0x07, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uae_u8 gvp_ide1_controller_autoconfig[16] = { 0xd1, 0x08, 0x00, 0x00, 0x07, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00 };

addrbank *gvp_ide_rom_autoconfig_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);
	int roms[2];
	const uae_u8 *autoconfig;

	if (ISCPUBOARD(BOARD_GVP, BOARD_GVP_SUB_A3001SI)) {
		ide->bank = &gvp_ide_rom_bank;
		autoconfig = gvp_ide1_controller_autoconfig;
		init_ide(ide, GVP_IDE, true, false);
		ide->rom_size = 8192;
		gvp_ide_controller_board->intena = true;
		ide->intena = true;
		gvp_ide_controller_board->configured = -1;
		roms[0] = 114;
		roms[1] = -1;
	} else {
		ide->bank = &gvp_ide_rom_bank;
		autoconfig = gvp_ide2_rom_autoconfig;
		ide->rom_size = 16384;
		roms[0] = -1;
	}
	ide->configured = 0;
	ide->mask = 65536 - 1;
	ide->type = GVP_IDE;
	ide->configured = 0;
	memset(ide->acmemory, 0xff, sizeof ide->acmemory);

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	ide->rom_mask = ide->rom_size - 1;
	struct zfile *z = read_device_from_romconfig(rc, roms);
	if (z) {
		for (int i = 0; i < 16; i++) {
			uae_u8 b = autoconfig[i];
			ew(ide, i * 4, b);
		}
		int size = zfile_fread(ide->rom, 1, ide->rom_size, z);
		zfile_fclose(z);
	}
	return ide->bank;
}

addrbank *gvp_ide_controller_autoconfig_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);

	init_ide(ide, GVP_IDE, true, false);
	ide->configured = 0;
	ide->bank = &gvp_ide_controller_bank;
	memset(ide->acmemory, 0xff, sizeof ide->acmemory);
	for (int i = 0; i < 16; i++) {
		uae_u8 b = gvp_ide2_controller_autoconfig[i];
		ew(ide, i * 4, b);
	}
	return ide->bank;
}

void gvp_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	struct ide_hdf *ide;

	if (!allocide(&gvp_ide_rom_board, rc, ch))
		return;
	if (!allocide(&gvp_ide_controller_board, rc, ch))
		return;
	ide = add_ide_unit (&idecontroller_drive[(GVP_IDE + ci->controller_type_unit) * 2], 2, ch, ci, rc);
}

static const uae_u8 alf_autoconfig[16] = { 0xd1, 6, 0x00, 0x00, 0x08, 0x2c, 0x00, 0x00, 0x00, 0x00, ALF_ROM_OFFSET >> 8, ALF_ROM_OFFSET & 0xff };
static const uae_u8 alfplus_autoconfig[16] = { 0xd1, 38, 0x00, 0x00, 0x08, 0x2c, 0x00, 0x00, 0x00, 0x00, ALF_ROM_OFFSET >> 8, ALF_ROM_OFFSET & 0xff };

addrbank *alf_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);
	int roms[2];
	bool alfplus = cfgfile_board_enabled(&currprefs, ROMTYPE_ALFAPLUS, 0);

	if (!ide)
		return &expamem_null;

	ide->configured = 0;

	roms[0] = alfplus ? 118 : 117;
	roms[1] = -1;

	ide->configured = 0;
	ide->bank = &ide_bank_generic;
	ide->type = ALF_IDE;
	ide->rom_size = 32768 * 6;
	ide->userdata = alfplus;
	ide->intena = alfplus;
	ide->mask = 65536 - 1;

	memset(ide->acmemory, 0xff, sizeof ide->acmemory);

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	ide->rom_mask = ide->rom_size - 1;

	for (int i = 0; i < 16; i++) {
		uae_u8 b = alfplus ? alfplus_autoconfig[i] : alf_autoconfig[i];
		ew(ide, i * 4, b);
	}

	if (!rc->autoboot_disabled) {
		struct zfile *z = read_device_from_romconfig(rc, roms);
		if (z) {
			for (int i = 0; i < 0x1000 / 2; i++) {
				uae_u8 b;
				zfile_fread(&b, 1, 1, z);
				ide->rom[ALF_ROM_OFFSET + i * 4 + 0] = b;
				zfile_fread(&b, 1, 1, z);
				ide->rom[ALF_ROM_OFFSET + i * 4 + 2] = b;
			}
			for (int i = 0; i < 32768 - 0x1000; i++) {
				uae_u8 b;
				zfile_fread(&b, 1, 1, z);
				ide->rom[0x2000 + i * 4 + 1] = b;
				zfile_fread(&b, 1, 1, z);
				ide->rom[0x2000 + i * 4 + 3] = b;
			}
			zfile_fclose(z);
		}
	}
	return ide->bank;
}

void alf_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	add_ide_standard_unit(ch, ci, rc, alf_board, ALF_IDE, false, false);
}

// prod 0x22 = IDE + SCSI
// prod 0x23 = SCSI only
// prod 0x33 = IDE only

const uae_u8 apollo_autoconfig[16] = { 0xd2, 0x23, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, APOLLO_ROM_OFFSET >> 8, APOLLO_ROM_OFFSET & 0xff };
const uae_u8 apollo_autoconfig_cpuboard[16] = { 0xd2, 0x23, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, APOLLO_ROM_OFFSET >> 8, APOLLO_ROM_OFFSET & 0xff };
const uae_u8 apollo_autoconfig_cpuboard_060[16] = { 0xd2, 0x23, 0x00, 0x00, 0x22, 0x22, 0x00, 0x00, 0x00, 0x02, APOLLO_ROM_OFFSET >> 8, APOLLO_ROM_OFFSET & 0xff };

static addrbank *apollo_init(struct romconfig *rc, bool cpuboard)
{
	struct ide_board *ide = getide(rc);
	int roms[2];
	const uae_u8 *autoconfig;

	if (!ide)
		return &expamem_null;

	roms[0] = -1;
	ide->configured = 0;
	ide->bank = &ide_bank_generic;
	ide->rom_size = 32768;
	ide->mask = 131072 - 1;
	ide->type = APOLLO_IDE;

	memset(ide->acmemory, 0xff, sizeof ide->acmemory);

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	ide->rom_mask = ide->rom_size - 1;
	autoconfig = apollo_autoconfig;
	if (cpuboard) {
		if (currprefs.cpu_model == 68060)
			autoconfig = apollo_autoconfig_cpuboard_060;
		else
			autoconfig = apollo_autoconfig_cpuboard;
	}
	struct zfile *z = read_device_from_romconfig(rc, roms);
	for (int i = 0; i < 16; i++) {
		uae_u8 b = autoconfig[i];
		ew(ide, i * 4, b);
	}
	if (z) {
		int len = zfile_size(z);
		// skip 68060 $f0 ROM block
		if (len >= 65536)
			zfile_fseek(z, 32768, SEEK_SET);
		for (int i = 0; i < 32768; i++) {
			uae_u8 b;
			zfile_fread(&b, 1, 1, z);
			ide->rom[i] = b;
		}
		zfile_fclose(z);
	}
	return ide->bank;
}

addrbank *apollo_init_hd(struct romconfig *rc)
{
	return apollo_init(rc, false);
}
addrbank *apollo_init_cpu(struct romconfig *rc)
{
	return apollo_init(rc, true);
}

void apollo_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	add_ide_standard_unit(ch, ci, rc, apollo_board, APOLLO_IDE, false, false);
}

addrbank *masoboshi_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);
	int roms[2];

	if (!ide)
		return &expamem_null;

	ide->configured = 0;

	roms[0] = 120;
	roms[1] = -1;
	ide->configured = 0;
	ide->bank = &ide_bank_generic;
	ide->type = MASOBOSHI_IDE;
	ide->rom_size = 65536;
	ide->mask = 65536 - 1;
	ide->subtype = 0;

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	memset(ide->acmemory, 0xff, sizeof ide->acmemory);
	ide->rom_mask = ide->rom_size - 1;
	struct zfile *z = read_device_from_romconfig(rc, roms);
	if (z) {
		int len = zfile_size(z);
		for (int i = 0; i < 32768; i++) {
			uae_u8 b;
			zfile_fread(&b, 1, 1, z);
			ide->rom[i * 2 + 0] = b;
			ide->rom[i * 2 + 1] = 0xff;
		}
		zfile_fclose(z);
		ide->subtype = rc->subtype;
		if (rc && rc->autoboot_disabled)
			memcpy(ide->acmemory, ide->rom + 0x100, sizeof ide->acmemory);
		else
			memcpy(ide->acmemory, ide->rom + 0x000, sizeof ide->acmemory);
	}
	// init SCSI part
	ncr_masoboshi_autoconfig_init(rc);
	return ide->bank;
}

static void masoboshi_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	add_ide_standard_unit(ch, ci, rc, masoboshi_board, MASOBOSHI_IDE, true, false);
}

void masoboshi_add_idescsi_unit (int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	if (ch < 0) {
		masoboshi_add_ide_unit(ch, ci, rc);
		masoboshi_add_scsi_unit(ch, ci, rc);
	} else {
		if (ci->controller_type < HD_CONTROLLER_TYPE_SCSI_FIRST)
			masoboshi_add_ide_unit(ch, ci, rc);
		else
			masoboshi_add_scsi_unit(ch, ci, rc);
	}
}

static const uae_u8 adide_autoconfig[16] = { 0xd1, 0x02, 0x00, 0x00, 0x08, 0x17, 0x00, 0x00, 0x00, 0x00, ADIDE_ROM_OFFSET >> 8, ADIDE_ROM_OFFSET & 0xff };

addrbank *adide_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);
	int roms[2];

	roms[0] = 129;
	roms[1] = -1;
	ide->configured = 0;
	ide->keepautoconfig = false;
	ide->bank = &ide_bank_generic;
	ide->rom_size = 32768;
	ide->mask = 65536 - 1;

	memset(ide->acmemory, 0xff, sizeof ide->acmemory);

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	ide->rom_mask = ide->rom_size - 1;
	struct zfile *z = read_device_from_romconfig(rc, roms);
	for (int i = 0; i < 16; i++) {
		uae_u8 b = adide_autoconfig[i];
		ew(ide, i * 4, b);
	}
	if (z) {
		for (int i = 0; i < 16384; i++) {
			uae_u8 b;
			zfile_fread(&b, 1, 1, z);
			ide->rom[i * 2 + 0] = b;
			ide->rom[i * 2 + 1] = 0xff;
		}
		zfile_fclose(z);
	}
	return ide->bank;
}

void adide_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	add_ide_standard_unit(ch, ci, rc, adide_board, ADIDE_IDE, true, true);
}

addrbank *mtec_init(struct romconfig *rc)
{
	struct ide_board *ide = getide(rc);
	int roms[2];

	roms[0] = 130;
	roms[1] = -1;
	ide->configured = 0;
	ide->bank = &ide_bank_generic;
	ide->rom_size = 32768;
	ide->mask = 65536 - 1;

	memset(ide->acmemory, 0xff, sizeof ide->acmemory);

	ide->rom = xcalloc(uae_u8, ide->rom_size);
	memset(ide->rom, 0xff, ide->rom_size);
	ide->rom_mask = ide->rom_size - 1;
	struct zfile *z = read_device_from_romconfig(rc, roms);
	if (z) {
		if (!rc->autoboot_disabled)
			zfile_fseek(z, 16384, SEEK_SET);
		for (int i = 0; i < 16384; i++) {
			uae_u8 b;
			zfile_fread(&b, 1, 1, z);
			ide->rom[i * 2 + 0] = b;
			ide->rom[i * 2 + 1] = 0xff;
		}
		zfile_fclose(z);
		memcpy(ide->acmemory, ide->rom, sizeof ide->acmemory);
	}
	return ide->bank;
}

void mtec_add_ide_unit(int ch, struct uaedev_config_info *ci, struct romconfig *rc)
{
	add_ide_standard_unit(ch, ci, rc, mtec_board, MTEC_IDE, false, false);
}
