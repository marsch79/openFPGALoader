// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2021 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 */

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "display.hpp"
#include "flashInterface.hpp"
#include "spiFlash.hpp"

FlashInterface::FlashInterface():_spif_verbose(0), _spif_rd_burst(0),
	_spif_verify(false), _skip_load_bridge(false)
{}

FlashInterface::FlashInterface(const std::string &filename, int8_t verbose,
		uint32_t rd_burst, bool verify, bool skip_load_bridge,
		bool skip_reset):
	_spif_verbose(verbose), _spif_rd_burst(rd_burst),
	_spif_verify(verify), _skip_load_bridge(skip_load_bridge),
	_skip_reset(skip_reset), _spif_filename(filename)
{}

/* Lattice Nexus dual-boot backup JUMP, see flashInterface.hpp */
#define DUAL_BOOT_JUMP_24BIT 0x00FFFF00u
#define DUAL_BOOT_JUMP_LEN   256u

static uint32_t next_pow2(uint32_t v)
{
	uint32_t p = 0x10000; /* at least one 64 KB block */
	while (p < v && p < 0x80000000u)
		p <<= 1;
	return p;
}

bool dual_boot_jump_plan(const std::vector<FlashDataSection> &sections,
		uint32_t flash_capacity, uint8_t page[256], uint32_t *source_addr,
		uint32_t *target_addr, std::string &err)
{
	uint32_t mcs_end = 0;
	for (const FlashDataSection &sec : sections) {
		uint32_t end = sec.getStartAddr() + static_cast<uint32_t>(sec.getLength());
		if (end > mcs_end)
			mcs_end = end;
	}
	if (mcs_end == 0) {
		err = "MCS is empty";
		return false;
	}
	if (flash_capacity == 0 || (flash_capacity & (flash_capacity - 1)) != 0) {
		err = "flash size unknown (part not in spiFlashdb), cannot place the dual-boot JUMP";
		return false;
	}
	const uint32_t mcs_size = next_pow2(mcs_end);
	if (mcs_size > flash_capacity) {
		err = "MCS does not fit the flash";
		return false;
	}

	*source_addr = DUAL_BOOT_JUMP_24BIT & (mcs_size - 1);
	*target_addr = DUAL_BOOT_JUMP_24BIT & (flash_capacity - 1);

	/* collect the JUMP page from the MCS; anything not in a section is erased */
	bool present = false;
	for (uint32_t i = 0; i < DUAL_BOOT_JUMP_LEN; i++)
		page[i] = 0xff;
	for (const FlashDataSection &sec : sections) {
		const uint32_t start = sec.getStartAddr();
		const std::vector<uint8_t> &data = sec.getRecord();
		for (uint32_t i = 0; i < DUAL_BOOT_JUMP_LEN; i++) {
			const uint32_t a = *source_addr + i;
			if (a >= start && a < start + data.size()) {
				page[i] = data[a - start];
				present = true;
			}
		}
	}
	/* the parser has already reversed the MCS bit order (Lattice): the page
	 * holds what the flash will hold, a plain "LSCC" signature */
	if (!present || page[0] != 'L' || page[1] != 'S' || page[2] != 'C' ||
			page[3] != 'C') {
		char mess[128];
		snprintf(mess, sizeof(mess),
			"MCS has no dual-boot JUMP record (\"LSCC\") at 0x%06x", *source_addr);
		err = mess;
		return false;
	}
	if (*target_addr != *source_addr && *target_addr < mcs_end) {
		err = "dual-boot JUMP target overlaps MCS data";
		return false;
	}
	return true;
}

/* place the MCS's JUMP page where the FPGA reads it, and read it back */
static bool write_dual_boot_jump(SPIFlash &flash,
		const std::vector<FlashDataSection> &sections, int rd_burst)
{
	uint8_t page[DUAL_BOOT_JUMP_LEN];
	uint32_t source_addr, target_addr;
	std::string err;
	char mess[256];

	if (!dual_boot_jump_plan(sections, flash.capacity(), page, &source_addr,
			&target_addr, err)) {
		printError("Dual-boot JUMP: " + err);
		return false;
	}
	if (target_addr == source_addr) {
		snprintf(mess, sizeof(mess),
			"Dual-boot JUMP: flash size matches the MCS, JUMP at 0x%06x already in place",
			target_addr);
		printInfo(mess);
		return true;
	}

	snprintf(mess, sizeof(mess),
		"Dual-boot JUMP: copying 0x%06x to 0x%06x (last page of %u MiB flash)",
		source_addr, target_addr, flash.capacity() >> 20);
	printInfo(mess);
	FlashDataSection jump(target_addr);
	jump.append(page, DUAL_BOOT_JUMP_LEN);
	std::vector<FlashDataSection> jump_sections = {jump};
	if (!flash.erase_and_prog(jump_sections))
		return false;
	/* always verified: this page is what an interrupted update falls back on */
	return flash.verify(target_addr, page, DUAL_BOOT_JUMP_LEN, rd_burst);
}

/* spiFlash generic acces */
bool FlashInterface::detect_flash()
{
	bool ret = true;

	printInfo("Detect flash:");

	/* move device to spi access */
	if (!prepare_flash_access()) {
		printError("Fail");
		return false;
	}

	/* spi flash access */
	try {
		// instanciate call (display flash ID is automatic)
		SPIFlash flash(this, false, _spif_verbose);
		// display status register
		flash.display_status_reg();

		printSuccess("Done");
	} catch (std::exception &e) {
		printError("Fail");
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}

bool FlashInterface::protect_flash(uint32_t len)
{
	bool ret = true;
	printInfo("protect_flash:");

	/* move device to spi access */
	if (!prepare_flash_access()) {
		printError("Fail");
		return false;
	}

	/* spi flash access */
	try {
		SPIFlash flash(this, false, _spif_verbose);

		/* configure flash protection */
		ret = (flash.enable_protection(len) == 0);
		if (!ret)
			printError("Fail");
		else
			printSuccess("Done");
	} catch (std::exception &e) {
		printError("Fail");
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}

bool FlashInterface::unprotect_flash()
{
	bool ret = true;

	/* move device to spi access */
	if (!prepare_flash_access()) {
		printError("SPI Flash prepare access failed");
		return false;
	}

	/* spi flash access */
	try {
		SPIFlash flash(this, false, _spif_verbose);

		/* configure flash protection */
		printInfo("unprotect_flash:");
		ret = (flash.disable_protection() == 0);
		if (!ret)
			printError("Fail");
		else
			printSuccess("Done");
	} catch (std::exception &e) {
		printError("SPI Flash access failed: ", false);
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}

bool FlashInterface::set_quad_bit(bool set_quad)
{
	bool ret = true;

	/* move device to spi access */
	if (!prepare_flash_access()) {
		printError("SPI Flash prepare access failed");
		return false;
	}

	/* spi flash access */
	try {
		SPIFlash flash(this, false, _spif_verbose);

		/* configure flash protection */
		printInfo("set_quad_bit:");
		ret = flash.set_quad_bit(set_quad);
		if (!ret)
			printError("Fail");
		else
			printSuccess("Done");
	} catch (std::exception &e) {
		printError("SPI Flash access failed: ", false);
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}

bool FlashInterface::bulk_erase_flash()
{
	bool ret = true;
	printInfo("bulk_erase:");

	/* move device to spi access */
	if (!prepare_flash_access()) {
		printError("Fail");
		return false;
	}

	/* spi flash access */
	try {
		SPIFlash flash(this, false, _spif_verbose);

		/* bulk erase flash */
		ret = (flash.bulk_erase() == 0);
		if (!ret)
			printError("Fail");
		else
			printSuccess("Done");
	} catch (std::exception &e) {
		printError("Fail");
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}

bool FlashInterface::write(const std::vector<FlashDataSection>&sections,
		bool unprotect_flash, bool full_erase)
{
	bool ret = true;
	if (!prepare_flash_access())
		return false;

	/* test SPI */
	try {
		SPIFlash flash(this, unprotect_flash, _spif_verbose);
		restore_flash_access_frequency();
		flash.read_status_reg();
		if (!flash.erase_and_prog(sections, full_erase))
			ret = false;
		if (_spif_verify && ret) {
			for (const FlashDataSection &sec: sections) {
				if (sec.getLength() == 0)
					continue;
				if (!flash.verify(sec.getStartAddr(), sec.getRecord().data(),
						sec.getLength(), _spif_rd_burst)) {
					ret = false;
					break;
				}
			}
		}
		if (_dual_boot_jump_mirror && ret &&
				!write_dual_boot_jump(flash, sections, _spif_rd_burst))
			ret = false;
	} catch (std::exception &e) {
		printError(e.what());
		ret = false;
	}

	bool ret2 = post_flash_access();
	return ret && ret2;
}

bool FlashInterface::write(uint32_t offset, const uint8_t *data, uint32_t len,
		bool unprotect_flash)
{
	bool ret = true;
	if (!prepare_flash_access())
		return false;

	/* test SPI */
	try {
		SPIFlash flash(this, unprotect_flash, _spif_verbose);
		restore_flash_access_frequency();
		flash.read_status_reg();
		if (flash.erase_and_prog(offset, data, len) == -1)
			ret = false;
		if (_spif_verify && ret)
			ret = flash.verify(offset, data, len, _spif_rd_burst);
	} catch (std::exception &e) {
		printError(e.what());
		ret = false;
	}

	bool ret2 = post_flash_access();
	return ret && ret2;
}

bool FlashInterface::read(uint8_t *data, uint32_t base_addr, uint32_t len)
{
	bool ret = true;
	/* enable SPI flash access */
	if (!prepare_flash_access())
		return false;

	try {
		SPIFlash flash(this, false, _spif_verbose);
		ret = flash.read(base_addr, data, len);
	} catch (std::exception &e) {
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret == 0;
}

bool FlashInterface::dump(uint32_t base_addr, uint32_t len)
{
	bool ret = true;
	/* enable SPI flash access */
	if (!prepare_flash_access())
		return false;

	try {
		SPIFlash flash(this, false, _spif_verbose);
		ret = flash.dump(_spif_filename, base_addr, len, _spif_rd_burst);
	} catch (std::exception &e) {
		printError(e.what());
		ret = false;
	}

	/* reload bitstream */
	return post_flash_access() && ret;
}
