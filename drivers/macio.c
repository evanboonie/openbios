/*
 *   derived from mol/mol.c,
 *   Copyright (C) 2003, 2004 Samuel Rydh (samuel@ibrium.se)
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License
 *   version 2
 *
 */

#include "config.h"
#include "arch/common/nvram.h"
#include "packages/nvram.h"
#include "libopenbios/bindings.h"
#include "libc/byteorder.h"
#include "libc/vsprintf.h"

#include "drivers/drivers.h"
#include "macio.h"
#include "cuda.h"
#include "pmu.h"
#include "escc.h"
#include "drivers/pci.h"
#include "arch/ppc/processor.h"
#include "timer.h"

#define OW_IO_NVRAM_SIZE   0x00020000
#define OW_IO_NVRAM_OFFSET 0x00060000
#define OW_IO_NVRAM_SHIFT  4

#define NW_IO_NVRAM_SIZE   0x00004000
#define NW_IO_NVRAM_OFFSET 0xfff04000

#define IO_OPENPIC_SIZE    0x00040000
#define IO_OPENPIC_OFFSET  0x00040000

static char *nvram;

/*
 * powermac7_3's flash NVRAM (evanboonie/qemu-g5#33)
 *
 * With -drive if=pflash, QEMU's powermac7_3 puts an AMD-style flash at the
 * NVRAM's address, as on the real G5, and sets FW_CFG_PPC_NVRAM_FLAT. Mac OS
 * X drives it with AppleFlashNVRAMAMD, which matches "amd-0137" and keeps a
 * Core99 image in it: two 8 KB banks, each a 32-byte header and then the
 * CHRP partitions. The header has signature 0x5a, a CHRP checksum over its
 * first 16 bytes, an Adler-32 of the bank from byte 20, and a generation.
 * The valid bank with the higher generation is the current one, and a write
 * goes to the other bank, with the generation one higher (read in
 * AppleFlashNVRAM-105.4.0: AppleFlashNVRAM.h:75-92, AppleFlashNVRAM.cpp:79-95,
 * :177-231, :252-308).
 *
 * Here OpenBIOS's own partitions, "common" among them, fill the rest of the
 * current bank. The header's length is 2, so that a partition walk such as
 * IODTNVRAM's steps from it to the first of them. The flash ignores plain
 * stores, so a bank is erased and programmed with AMD commands, and checked
 * by reading it back.
 *
 * OpenBIOS runs from a RAM copy of 0xfff00000-0xffffffff, which it maps over
 * that range (arch/ppc/qemu/ofmem.c), so a load or store at the flash's
 * address reaches the copy, not the device. The flash is reached with data
 * translation off instead, one byte at a time. That relies on QEMU, which
 * doesn't model caching: on a real 970, real-mode data accesses are cacheable
 * unless HID4's real-mode cache-inhibit bit is set.
 */
#define FLASH_BANK_SIZE        0x2000
#define FLASH_HDR_SIZE         0x20
#define FLASH_HDR_SIG          0x5a
#define FLASH_HDR_ADLER        16
#define FLASH_HDR_GEN          20
#define FLASH_DATA_SIZE        (FLASH_BANK_SIZE - FLASH_HDR_SIZE)
#define FLASH_POLL_SECONDS     2

/* AMD command addresses, relative to the bank, and commands */
#define AMD_ADDR1              0x555
#define AMD_ADDR2              0x2aa
#define AMD_UNLOCK1            0xaa
#define AMD_UNLOCK2            0x55
#define AMD_BYPASS             0x20
#define AMD_ERASE              0x80
#define AMD_SECTOR_ERASE       0x30
#define AMD_PROGRAM            0xa0
#define AMD_BYPASS_RESET1      0x90
#define AMD_BYPASS_RESET2      0x00
#define AMD_RESET              0xf0
#define AMD_DQ6                0x40
#define AMD_DQ5                0x20

/* The timebase's frequency, from drivers/timer.c */
extern unsigned long timer_freq;

static int nvram_flash = -1;
static unsigned long flash_base;
static int flash_cur = -1;
static uint32_t flash_gen;

static int macio_nvram_is_flash(void)
{
	if (nvram_flash < 0) {
		nvram_flash = is_powermac7_3() &&
			fw_cfg_read_i32(FW_CFG_PPC_NVRAM_FLAT);
	}
	return nvram_flash;
}

#ifdef __powerpc64__
#define FLASH_MTMSR "mtmsrd"
#else
#define FLASH_MTMSR "mtmsr"
#endif

/* Read a byte at physical address pa, with data translation off */
static uint8_t flash_in8(unsigned long pa)
{
	unsigned long msr, tmp;
	unsigned long val;

	asm volatile("mfmsr %0\n\t"
		     "andc %1,%0,%4\n\t"
		     "sync\n\t"
		     FLASH_MTMSR " %1\n\t"
		     "isync\n\t"
		     "lbz %2,0(%3)\n\t"
		     "sync\n\t"
		     FLASH_MTMSR " %0\n\t"
		     "isync"
		     : "=&r" (msr), "=&r" (tmp), "=&r" (val)
		     : "b" (pa), "r" ((unsigned long)(MSR_DR | MSR_EE))
		     : "memory");
	return val;
}

/* Write a byte at physical address pa, with data translation off */
static void flash_out8(unsigned long pa, uint8_t val)
{
	unsigned long msr, tmp;

	asm volatile("mfmsr %0\n\t"
		     "andc %1,%0,%4\n\t"
		     "sync\n\t"
		     FLASH_MTMSR " %1\n\t"
		     "isync\n\t"
		     "stb %3,0(%2)\n\t"
		     "sync\n\t"
		     FLASH_MTMSR " %0\n\t"
		     "isync"
		     : "=&r" (msr), "=&r" (tmp)
		     : "b" (pa), "r" ((unsigned long)val),
		       "r" ((unsigned long)(MSR_DR | MSR_EE))
		     : "memory");
}

static uint64_t flash_timebase(void)
{
	unsigned long hi, lo, hi2;

	do {
		asm volatile("mftbu %0" : "=r" (hi));
		asm volatile("mftb %0" : "=r" (lo));
		asm volatile("mftbu %0" : "=r" (hi2));
	} while (hi != hi2);
	return ((uint64_t)(uint32_t)hi << 32) | (uint32_t)lo;
}

/*
 * Wait for an erase or a program at pa to finish: DQ6 toggles on each read
 * while it runs. DQ5 set while DQ6 still toggles is the part's own timeout.
 * Returns 0 when done, or -1 after resetting the part.
 */
static int flash_wait(unsigned long pa)
{
	uint64_t start = flash_timebase();
	uint64_t limit = (uint64_t)timer_freq * FLASH_POLL_SECONDS;
	uint8_t a, b;

	for (;;) {
		a = flash_in8(pa);
		b = flash_in8(pa);
		if (((a ^ b) & AMD_DQ6) == 0) {
			return 0;
		}
		if (b & AMD_DQ5) {
			/* DQ6 may have stopped just as DQ5 was set */
			a = flash_in8(pa);
			b = flash_in8(pa);
			if (((a ^ b) & AMD_DQ6) == 0) {
				return 0;
			}
			break;
		}
		if (flash_timebase() - start > limit) {
			break;
		}
	}
	flash_out8(pa, AMD_RESET);
	return -1;
}

static void flash_unlock(unsigned long bank)
{
	flash_out8(bank + AMD_ADDR1, AMD_UNLOCK1);
	flash_out8(bank + AMD_ADDR2, AMD_UNLOCK2);
}

static int flash_erase_bank(unsigned long bank)
{
	int i;

	flash_unlock(bank);
	flash_out8(bank + AMD_ADDR1, AMD_ERASE);
	flash_unlock(bank);
	flash_out8(bank, AMD_SECTOR_ERASE);
	/* The erase may start only after the 50 us sector erase window */
	udelay(60);
	if (flash_wait(bank)) {
		printk("flash NVRAM: erase at 0x%lx timed out\n", bank);
		return -1;
	}
	for (i = 0; i < FLASH_BANK_SIZE; i++) {
		if (flash_in8(bank + i) != 0xff) {
			printk("flash NVRAM: byte 0x%x at 0x%lx not erased\n", i, bank);
			return -1;
		}
	}
	return 0;
}

static int flash_program_bank(unsigned long bank, const uint8_t *data)
{
	int i, err = 0;

	/* Unlock bypass: one command cycle per byte, not three */
	flash_unlock(bank);
	flash_out8(bank + AMD_ADDR1, AMD_BYPASS);
	for (i = 0; i < FLASH_BANK_SIZE; i++) {
		flash_out8(bank + AMD_ADDR1, AMD_PROGRAM);
		flash_out8(bank + i, data[i]);
		if (flash_wait(bank + i)) {
			printk("flash NVRAM: program at 0x%lx timed out\n", bank + i);
			err = -1;
			break;
		}
	}
	flash_out8(bank, AMD_BYPASS_RESET1);
	flash_out8(bank, AMD_BYPASS_RESET2);
	if (err) {
		return err;
	}
	for (i = 0; i < FLASH_BANK_SIZE; i++) {
		if (flash_in8(bank + i) != data[i]) {
			printk("flash NVRAM: byte 0x%x at 0x%lx reads back wrong\n", i, bank);
			return -1;
		}
	}
	return 0;
}

/* The CHRP header checksum over bytes 0-15, leaving out byte 1 */
static uint8_t flash_hdr_checksum(const uint8_t *p)
{
	unsigned int sum = p[0];
	int i;

	for (i = 2; i < 16; i++) {
		sum += p[i];
		if (sum > 0xff) {
			sum = (sum + 1) & 0xff;
		}
	}
	return sum;
}

static uint32_t flash_adler32(const uint8_t *p, int len)
{
	uint32_t a = 1, b = 0;
	int i;

	for (i = 0; i < len; i++) {
		a = (a + p[i]) % 65521;
		b = (b + a) % 65521;
	}
	return (b << 16) | a;
}

static uint32_t flash_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static void flash_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static unsigned long flash_bank(int n)
{
	return flash_base + n * FLASH_BANK_SIZE;
}

/* Read bank n into buf. Returns 1 if its header is valid, with its generation */
static int flash_read_bank(int n, uint8_t *buf, uint32_t *gen)
{
	unsigned long bank = flash_bank(n);
	int i;

	for (i = 0; i < FLASH_BANK_SIZE; i++) {
		buf[i] = flash_in8(bank + i);
	}
	if (buf[0] != FLASH_HDR_SIG ||
	    buf[1] != flash_hdr_checksum(buf) ||
	    flash_get_be32(buf + FLASH_HDR_ADLER) !=
	    flash_adler32(buf + FLASH_HDR_GEN, FLASH_BANK_SIZE - FLASH_HDR_GEN)) {
		return 0;
	}
	*gen = flash_get_be32(buf + FLASH_HDR_GEN);
	return 1;
}

/*
 * Mac OS X reads the numbers in the OF variables as C does, with
 * strtol(.., 0), and writes them back as -1, in decimal below 1000, or else
 * in hex with "0x" (read: IODTNVRAM::convertPropToObject() and
 * convertObjectToProp(), iokit/Kernel/IONVRAM.cpp, xnu-1228.15.4).
 * OpenBIOS's int-configs hold hex digits with no prefix (exec-int-conf in
 * forth/admin/nvram.fs), so Mac OS X would read load-base's "4000000" as
 * decimal, and OpenBIOS can't parse the "0x3d0900" it writes back. On the
 * flash, the "common" partition holds them in Mac OS X's form, converted as
 * a bank is read and written.
 */
#define FLASH_SIG_SYSTEM       0x70

static int flash_is_int_config(const char *name, int len)
{
	char buf[64];

	if (len <= 0 || len >= (int)sizeof(buf)) {
		return 0;
	}
	memcpy(buf, name, len);
	buf[len] = '\0';
	push_str(buf);
	fword("int-config?");
	return POP();
}

/*
 * Write an int-config's value in the other side's form into out. Returns 0
 * if it isn't a number, to be left as it is.
 */
static int flash_convert_int(const char *val, char *out, int outlen,
			     int to_macos)
{
	char *end;
	uint32_t v;

	if (to_macos) {
		/* OpenBIOS: hex digits, maybe after a minus sign */
		v = strtol(val, &end, 16);
	} else {
		v = strtol(val, &end, 0);
	}
	if (end == val || *end) {
		return 0;
	} else if (v == 0xffffffff) {
		snprintf(out, outlen, "-1");
	} else if (!to_macos) {
		snprintf(out, outlen, "%x", v);
	} else if (v < 1000) {
		snprintf(out, outlen, "%u", v);
	} else {
		snprintf(out, outlen, "0x%x", v);
	}
	return 1;
}

/*
 * Say that an entry was dropped, by its name alone, since a value such as
 * platform-uuid's can hold a MAC address. printk() ignores a precision for
 * "%s", so the name is copied out.
 */
static void flash_dropped(const char *str, int namelen, const char *why)
{
	char name[32];

	if (namelen > (int)sizeof(name) - 1) {
		namelen = sizeof(name) - 1;
	}
	memcpy(name, str, namelen);
	name[namelen] = '\0';
	printk("flash NVRAM: dropped an entry%s%s, which %s\n",
	       namelen ? " named " : "", name, why);
}

/*
 * Convert the int-configs' values in the system partition named "common", in
 * the partitions at data, from OpenBIOS's form to Mac OS X's or back, one
 * entry at a time. An entry that doesn't fit the partition is dropped, with a
 * message naming it, and the others are still converted, so that no number is
 * left in the other side's form.
 *
 * Mac OS X may fill the partition to its last byte, with no empty string to
 * end the list. OpenBIOS's nvram-load-configs needs one, so when reading, an
 * entry that leaves no room for it is dropped as well.
 */
static void flash_convert_configs(uint8_t *data, int size, int to_macos)
{
	uint8_t *p = data, *cfg = NULL, *out = NULL;
	int len, cfg_size = 0, i, o, last = -1;
	char val[32];

	while (p + 16 <= data + size) {
		len = ((p[2] << 8) | p[3]) * 16;
		if (len < 16 || p + len > data + size) {
			return;
		}
		if (p[0] == FLASH_SIG_SYSTEM &&
		    !strncmp((char *)p + 4, "common", 12)) {
			cfg = p + 16;
			cfg_size = len - 16;
			break;
		}
		p += len;
	}
	if (!cfg || !(out = malloc(cfg_size))) {
		return;
	}
	memset(out, 0, cfg_size);
	for (i = 0, o = 0; i < cfg_size && cfg[i]; ) {
		char *str = (char *)cfg + i, *eq;
		int slen = strnlen(str, cfg_size - i);
		int namelen, n, conv;

		if (i + slen >= cfg_size) {
			flash_dropped(str, 0, "has no end");
			break;
		}
		eq = strchr(str, '=');
		/* The name, with no value: only the name is ever printed */
		namelen = eq ? eq - str : 0;
		conv = eq && flash_is_int_config(str, namelen) &&
		       flash_convert_int(eq + 1, val, sizeof(val), to_macos);
		n = conv ? namelen + 1 + (int)strlen(val) : slen;
		/* The entry and its NUL, and on writes the list's NUL */
		if (o + n + (to_macos ? 2 : 1) > cfg_size) {
			flash_dropped(str, namelen, "doesn't fit");
		} else {
			if (conv) {
				memcpy(out + o, str, namelen + 1);
				memcpy(out + o + namelen + 1, val, strlen(val));
			} else {
				memcpy(out + o, str, n);
			}
			last = o;
			o += n + 1;
		}
		i += slen + 1;
	}
	if (o >= cfg_size && last >= 0) {
		/* No room left for the empty string that ends the list */
		char *eq = strchr((char *)out + last, '=');
		int namelen = eq ? eq - ((char *)out + last) : 0;

		flash_dropped((char *)out + last, namelen,
			      "leaves no room for the list's end");
		memset(out + last, 0, cfg_size - last);
	}
	memcpy(cfg, out, cfg_size);
	free(out);
}

/* Fill buf with the current bank's partitions, or zeros if neither is valid */
static void flash_nvram_get(char *buf)
{
	uint8_t *bank = malloc(FLASH_BANK_SIZE);
	uint32_t gen[2] = { 0, 0 };
	int valid[2];

	if (!bank) {
		printk("flash NVRAM: out of memory\n");
		memset(buf, 0, FLASH_DATA_SIZE);
		return;
	}
	valid[0] = flash_read_bank(0, bank, &gen[0]);
	valid[1] = flash_read_bank(1, bank, &gen[1]);
	if (valid[0] && (!valid[1] || gen[0] > gen[1])) {
		flash_cur = 0;
	} else if (valid[1]) {
		flash_cur = 1;
	} else {
		flash_cur = -1;
	}
	if (flash_cur < 0) {
		printk("flash NVRAM: no valid bank\n");
		memset(buf, 0, FLASH_DATA_SIZE);
	} else {
		flash_gen = gen[flash_cur];
		flash_read_bank(flash_cur, bank, &gen[flash_cur]);
		memcpy(buf, bank + FLASH_HDR_SIZE, FLASH_DATA_SIZE);
		flash_convert_configs((uint8_t *)buf, FLASH_DATA_SIZE, 0);
		printk("flash NVRAM: bank %d, generation %u\n", flash_cur,
		       flash_gen);
	}
	free(bank);
}

/*
 * Write buf's partitions into the bank that isn't current, with the next
 * generation, unless the current bank already holds them.
 */
static void flash_nvram_put(char *buf)
{
	uint8_t *img = malloc(FLASH_BANK_SIZE);
	uint8_t *cur = malloc(FLASH_BANK_SIZE);
	uint32_t gen;
	int next;

	if (!img || !cur) {
		printk("flash NVRAM: out of memory, not written\n");
		goto out;
	}
	memcpy(img + FLASH_HDR_SIZE, buf, FLASH_DATA_SIZE);
	flash_convert_configs(img + FLASH_HDR_SIZE, FLASH_DATA_SIZE, 1);
	if (flash_cur >= 0 && flash_read_bank(flash_cur, cur, &gen) &&
	    !memcmp(cur + FLASH_HDR_SIZE, img + FLASH_HDR_SIZE,
		    FLASH_DATA_SIZE)) {
		goto out;
	}

	memset(img, 0, FLASH_HDR_SIZE);
	img[0] = FLASH_HDR_SIG;
	img[2] = (FLASH_HDR_SIZE / 16) >> 8;
	img[3] = FLASH_HDR_SIZE / 16;
	strncpy((char *)img + 4, "nvram", 12);
	gen = flash_cur >= 0 ? flash_gen + 1 : 1;
	flash_put_be32(img + FLASH_HDR_GEN, gen);
	flash_put_be32(img + FLASH_HDR_ADLER,
		       flash_adler32(img + FLASH_HDR_GEN,
				     FLASH_BANK_SIZE - FLASH_HDR_GEN));
	img[1] = flash_hdr_checksum(img);

	next = flash_cur == 0 ? 1 : 0;
	printk("flash NVRAM: writing bank %d, generation %u\n", next, gen);
	if (flash_erase_bank(flash_bank(next)) ||
	    flash_program_bank(flash_bank(next), img)) {
		printk("flash NVRAM: bank %d not written\n", next);
		goto out;
	}
	flash_cur = next;
	flash_gen = gen;
out:
	free(cur);
	free(img);
}

/* Set the variables in QEMU's etc/prom-env (-prom-env) over the flash's */
static void flash_nvram_prom_env(void)
{
	uint32_t size;
	char *env = fw_cfg_read_file("etc/prom-env", &size);

	if (env) {
		nvconf_load_env(env, size);
		free(env);
	}
}

static int macio_nvram_shift(void)
{
	int nvram_flat;

        if (is_oldworld())
                return OW_IO_NVRAM_SHIFT;

	nvram_flat = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_FLAT);
	return nvram_flat ? 0 : 1;
}

int
macio_get_nvram_size(void)
{
	int shift;

	if (macio_nvram_is_flash())
		return FLASH_DATA_SIZE;
	shift = macio_nvram_shift();
        if (is_oldworld())
                return OW_IO_NVRAM_SIZE >> shift;
        else
                return NW_IO_NVRAM_SIZE >> shift;
}

static unsigned long macio_nvram_offset(void)
{
	unsigned long r;

	/* Hypervisor tells us where NVRAM lies */
	r = fw_cfg_read_i32(FW_CFG_PPC_NVRAM_ADDR);
	if (r)
		return r;

	/* Fall back to hardcoded addresses */
	if (is_oldworld())
		return OW_IO_NVRAM_OFFSET;

	return NW_IO_NVRAM_OFFSET;
}

static unsigned long macio_nvram_size(void)
{
	if (is_oldworld())
		return OW_IO_NVRAM_SIZE;
	else
		return NW_IO_NVRAM_SIZE;
}

void macio_nvram_init(const char *path, phys_addr_t addr)
{
	phandle_t chosen, aliases;
	phandle_t dnode;
	int props[2];
	char buf[64];
        unsigned long nvram_size, nvram_offset;

        nvram_offset = macio_nvram_offset();
        nvram_size = macio_nvram_size();

	nvram = (char*)addr + nvram_offset;
	flash_base = addr + nvram_offset;
	nvconf_init();
	if (macio_nvram_is_flash())
		flash_nvram_prom_env();
	snprintf(buf, sizeof(buf), "%s", path);
	dnode = nvram_init(buf);
	/* On the flash, one bank, as on the real G5 */
	set_int_property(dnode, "#bytes", macio_nvram_is_flash() ?
			 FLASH_BANK_SIZE : arch_nvram_size() );
	props[0] = __cpu_to_be32(nvram_offset);
	props[1] = __cpu_to_be32(nvram_size);
	set_property(dnode, "reg", (char *)&props, sizeof(props));
	set_property(dnode, "device_type", "nvram", 6);
	if (macio_nvram_is_flash()) {
		/* The real G5's, which AppleFlashNVRAMAMD matches */
		set_property(dnode, "compatible", "amd-0137\0nvram,flash", 21);
	} else {
		NEWWORLD(set_property(dnode, "compatible", "nvram,flash", 12));
	}

	chosen = find_dev("/chosen");
	snprintf(buf, sizeof(buf), "%s", get_path_from_ph(dnode));
	push_str(buf);
	fword("open-dev");
	set_int_property(chosen, "nvram", POP());

	aliases = find_dev("/aliases");
	set_property(aliases, "nvram", buf, strlen(buf) + 1);
}

#ifdef DUMP_NVRAM
static void
dump_nvram(void)
{
  int i, j;
  for (i = 0; i < 10; i++)
    {
      for (j = 0; j < 16; j++)
      printk ("%02x ", nvram[(i*16+j)<<4]);
      printk (" ");
      for (j = 0; j < 16; j++)
        if (isprint(nvram[(i*16+j)<<4]))
            printk("%c", nvram[(i*16+j)<<4]);
        else
          printk(".");
      printk ("\n");
      }
}
#endif


void
macio_nvram_put(char *buf)
{
	int i;
        unsigned int it_shift;

	if (macio_nvram_is_flash()) {
		flash_nvram_put(buf);
		return;
	}
	it_shift = macio_nvram_shift();
	for (i=0; i < arch_nvram_size(); i++)
		nvram[i << it_shift] = buf[i];
#ifdef DUMP_NVRAM
	printk("new nvram:\n");
	dump_nvram();
#endif
}

void
macio_nvram_get(char *buf)
{
	int i;
        unsigned int it_shift;

	if (macio_nvram_is_flash()) {
		flash_nvram_get(buf);
		return;
	}
	it_shift = macio_nvram_shift();
	for (i=0; i< arch_nvram_size(); i++)
                buf[i] = nvram[i << it_shift];

#ifdef DUMP_NVRAM
	printk("current nvram:\n");
	dump_nvram();
#endif
}

static void
openpic_init(const char *path, phys_addr_t addr)
{
        phandle_t dnode;
        int props[2];
        char buf[128];
        /* The real PowerMac7,3 names its MPIC mpic, and Mac OS X's
           MacRISC4PE looks it up as mac-io/mpic */
        const char *name = is_powermac7_3() ? "mpic" : "interrupt-controller";

        fword("new-device");
        push_str(name);
        fword("device-name");

        snprintf(buf, sizeof(buf), "%s/%s", path, name);
        dnode = find_dev(buf);
        set_property(dnode, "device_type", "open-pic", 9);
        set_property(dnode, "compatible", "chrp,open-pic", 14);
        set_property(dnode, "built-in", "", 0);
        props[0] = __cpu_to_be32(IO_OPENPIC_OFFSET);
        props[1] = __cpu_to_be32(IO_OPENPIC_SIZE);
        set_property(dnode, "reg", (char *)&props, sizeof(props));
        set_int_property(dnode, "#interrupt-cells", 2);
        set_int_property(dnode, "#address-cells", 0);
        set_property(dnode, "interrupt-controller", "", 0);
        set_int_property(dnode, "clock-frequency", 4166666);

        fword("finish-device");
}

DECLARE_UNNAMED_NODE(ob_macio, 0, sizeof(int));

/* ( str len -- addr ) */

static void
ob_macio_decode_unit(void *private)
{
	ucell addr;

	const char *arg = pop_fstr_copy();

	addr = strtol(arg, NULL, 16);

	free((char*)arg);

	PUSH(addr);
}

/*  ( addr -- str len ) */

static void
ob_macio_encode_unit(void *private)
{
	char buf[8];

	ucell addr = POP();

	snprintf(buf, sizeof(buf), "%x", addr);

	push_str(buf);
}

static void
ob_macio_dma_alloc(int *idx)
{
    call_parent_method("dma-alloc");
}

static void
ob_macio_dma_free(int *idx)
{
    call_parent_method("dma-free");
}

static void
ob_macio_dma_map_in(int *idx)
{
    call_parent_method("dma-map-in");
}

static void
ob_macio_dma_map_out(int *idx)
{
    call_parent_method("dma-map-out");
}

static void
ob_macio_dma_sync(int *idx)
{
    call_parent_method("dma-sync");
}

NODE_METHODS(ob_macio) = {
        { "decode-unit",	ob_macio_decode_unit	},
        { "encode-unit",	ob_macio_encode_unit	},
        { "dma-alloc",		ob_macio_dma_alloc	},
        { "dma-free",		ob_macio_dma_free		},
        { "dma-map-in",		ob_macio_dma_map_in	},
        { "dma-map-out",	ob_macio_dma_map_out	},
        { "dma-sync",		ob_macio_dma_sync		},
};

/*
 * powermac7_3's DART (evanboonie/qemu-g5#89)
 *
 * QEMU's powermac7_3 sets FW_CFG_PPC_DART when it has U3's DART at
 * 0xf8033000, in front of every device on the U3 bus (evanboonie/qemu-g5#84).
 * Then /u3 gets a dart child, after the real PowerMac7,3's. Mac OS X's
 * MacRISC4PE advertises a mapper when /u3/dart exists, and AppleDART matches
 * the node by name, takes U3's register layout from "u3-dart", and turns the
 * DART on, since Open Firmware leaves it off (read in AppleMacRISC4PE-204.0.9:
 * MacRISC4PE.cpp:225-233, and AppleMacRiscPCI-3.4: AppleDART.cpp:273-274).
 * The real node's claim-phys is left out: nothing in Mac OS X 10.5's or
 * Linux's DART code reads it.
 *
 * Without the key, QEMU has no DART to translate the addresses AppleDART
 * would hand out, so there's no node, as with the flash NVRAM's key.
 */
#define U3_DART_BASE    0xf8033000
#define U3_DART_SIZE    0x7000

static void u3_dart_init(void)
{
        phandle_t ph;
        int props[2];

        fword("new-device");
        push_str("dart");
        fword("device-name");
        push_str("dart");
        fword("device-type");

        ph = get_cur_dev();
        set_property(ph, "compatible", "u3-dart\0dart", 13);
        props[0] = __cpu_to_be32(U3_DART_BASE);
        props[1] = __cpu_to_be32(U3_DART_SIZE);
        set_property(ph, "reg", (char *)&props, sizeof(props));

        fword("finish-device");
}

void
ob_unin_init(void)
{
        phandle_t dnode;
        int props[2];

        fword("new-device");

        if (is_powermac7_3()) {
                /* The real PowerMac7,3's memory controller is U3, which Mac
                   OS X's AppleU3 matches by name */
                push_str("u3");
                fword("device-name");

                dnode = find_dev("/u3");
                set_property(dnode, "device_type", "memory-controller", 18);
                set_property(dnode, "compatible", "u3", 3);
                set_property(dnode, "model", "AAPL,U3", 8);
                set_int_property(dnode, "device-rev", 0x35);
        } else {
                push_str("uni-n");
                fword("device-name");

                dnode = find_dev("/uni-n");
                set_property(dnode, "device_type", "memory-controller", 18);
                set_property(dnode, "compatible", "uni-north", 10);
                set_int_property(dnode, "device-rev", 7);
        }
        props[0] = __cpu_to_be32(0xf8000000);
        props[1] = __cpu_to_be32(0x1000000);
        set_property(dnode, "reg", (char *)&props, sizeof(props));

        if (is_powermac7_3()) {
                /* As on the real PowerMac7,3, a child's reg is one address
                   cell and one size cell, and ranges maps U3's 256 MB at
                   0xf8000000 one to one. Mac OS X takes 2 address cells
                   where a parent gives none (IODeviceTreeSupport.cpp in
                   xnu-1228.15.4) */
                int ranges[3];

                set_int_property(dnode, "#address-cells", 1);
                set_int_property(dnode, "#size-cells", 1);
                /* The parent address is one cell, as the 32-bit build's root
                   has (arch/ppc/qemu/tree.fs) and as /u3's reg assumes:
                   CONFIG_PPC64 would need 2 */
                ranges[0] = __cpu_to_be32(0xf8000000);
                ranges[1] = __cpu_to_be32(0xf8000000);
                ranges[2] = __cpu_to_be32(0x10000000);
                set_property(dnode, "ranges", (char *)&ranges, sizeof(ranges));

                if (fw_cfg_read_i32(FW_CFG_PPC_DART)) {
                        u3_dart_init();
                }
        }

        fword("finish-device");
}

/* The K2 GPIO that QEMU's powermac7_3 wires to every CPU's TBEN
 * (qemu-g5#78), as an offset in the gpio node's block. Mac OS X's
 * MacRISC4CPU freezes the timebases through it while it copies CPU 0's
 * timebase to another CPU. */
#define POWERMAC7_3_TIMEBASE_ENABLE_GPIO 0x27

/* Builds the gpio node's timebase-enable child, with the platform-do
 * function that AppleGPIO publishes as platform-cpu-timebase-<phandle of
 * /cpus>, and points /cpus' platform-cpu-timebase at it. The encoding is the
 * PCIe Power Mac G5's: on demand (0x08000000), write GPIO (1), with value 0
 * and mask 4, so disabling drives the line low and enabling releases it.
 * Mac OS X looks for platform-cpu-timebase on /cpus before any other way of
 * syncing timebases, and, with no /u3/i2c pulsar either, starts only one
 * CPU without one. */
static void macio_timebase_enable_init(void)
{
    phandle_t cpus = find_dev("/cpus");
    phandle_t ph;
    u32 props[5];

    fword("new-device");
    push_str("timebase-enable");
    fword("device-name");
    push_str("gpio");
    fword("device-type");

    ph = get_cur_dev();
    set_int_property(ph, "reg", POWERMAC7_3_TIMEBASE_ENABLE_GPIO);
    set_property(ph, "compatible", "timebase-enable\0gpio", 21);

    props[0] = __cpu_to_be32(cpus);
    props[1] = __cpu_to_be32(0x08000000);
    props[2] = __cpu_to_be32(1);
    props[3] = __cpu_to_be32(0);
    props[4] = __cpu_to_be32(4);
    set_property(ph, "platform-do-cpu-timebase", (char *)props, sizeof(props));

    fword("finish-device");

    set_int_property(cpus, "platform-cpu-timebase", ph);
}

static void macio_gpio_init(const char *path)
{
    fword("new-device");

    push_str("gpio");
    fword("device-name");

    push_str("gpio");
    fword("device-type");

    PUSH(1);
    fword("encode-int");
    push_str("#address-cells");
    fword("property");

    PUSH(0);
    fword("encode-int");
    push_str("#size-cells");
    fword("property");

    push_str("mac-io-gpio");
    fword("encode-string");
    push_str("compatible");
    fword("property");

    PUSH(0x50);
    fword("encode-int");
    PUSH(0x30);
    fword("encode-int");
    fword("encode+");
    push_str("reg");
    fword("property");

    /* Build the extint-gpio1 for the PMU */
    fword("new-device");
    push_str("extint-gpio1");
    fword("device-name");
    PUSH(0x2f);
    fword("encode-int");
    PUSH(0x1);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    PUSH(0x9);
    fword("encode-int");
    push_str("reg");
    fword("property");
    push_str("keywest-gpio1");
    fword("encode-string");
    push_str("gpio");
    fword("encode-string");
    fword("encode+");
    push_str("compatible");
    fword("property");
    fword("finish-device");

    /* Build the programmer-switch */
    fword("new-device");
    push_str("programmer-switch");
    fword("device-name");
    push_str("programmer-switch");
    fword("encode-string");
    push_str("device_type");
    fword("property");
    PUSH(0x37);
    fword("encode-int");
    PUSH(0x0);
    fword("encode-int");
    fword("encode+");
    push_str("interrupts");
    fword("property");
    fword("finish-device");

    if (is_powermac7_3()) {
        macio_timebase_enable_init();
    }

    fword("finish-device");
}

void
ob_macio_heathrow_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    cuda_init(path, addr);
    macio_nvram_init(path, addr);
    escc_init(path, addr);
    macio_ide_init(path, addr, 2);

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}

void
ob_macio_keylargo_init(const char *path, phys_addr_t addr)
{
    phandle_t aliases;

    BIND_NODE_METHODS(get_cur_dev(), ob_macio);

    if (has_pmu()) {
        macio_gpio_init(path);
        pmu_init(path, addr);
    } else {
        cuda_init(path, addr);
    }

    escc_init(path, addr);
    macio_ide_init(path, addr, 2);
    openpic_init(path, addr);

    aliases = find_dev("/aliases");
    set_property(aliases, "mac-io", path, strlen(path) + 1);
}
