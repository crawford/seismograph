/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "sysincludes.h"

#include "cgptlib.h"
#include "cgptlib_internal.h"
#include "crc32.h"
#include "gpt.h"
#include "utility.h"


int CheckParameters(GptData *gpt)
{
	/* Currently, we only support 512-byte sectors. */
	if (gpt->sector_bytes != 512)
		return GPT_ERROR_INVALID_SECTOR_SIZE;

	/*
	 * Sector count of a drive should be reasonable. If the given value is
	 * too small to contain basic GPT structure (PMBR + Headers + Entries),
	 * the value is wrong.
	 */
	if (gpt->drive_sectors < (1 + 2 * (1 + GPT_ENTRIES_SECTORS)))
		return GPT_ERROR_INVALID_SECTOR_NUMBER;

	return GPT_SUCCESS;
}

uint32_t HeaderCrc(GptHeader *h)
{
	uint32_t crc32, original_crc32;

	/* Original CRC is calculated with the CRC field 0. */
	original_crc32 = h->header_crc32;
	h->header_crc32 = 0;
	crc32 = Crc32((const uint8_t *)h, h->size);
	h->header_crc32 = original_crc32;

	return crc32;
}

int CheckHeader(GptHeader *h, int is_secondary, uint64_t drive_sectors)
{
	if (!h)
		return 1;

	/*
	 * Make sure we're looking at a header of reasonable size before
	 * attempting to calculate CRC.
	 */
	if (Memcmp(h->signature, GPT_HEADER_SIGNATURE,
		   GPT_HEADER_SIGNATURE_SIZE) &&
	    Memcmp(h->signature, GPT_HEADER_SIGNATURE2,
		   GPT_HEADER_SIGNATURE_SIZE))
		return 1;
	if (h->revision != GPT_HEADER_REVISION)
		return 1;
	if (h->size < MIN_SIZE_OF_HEADER || h->size > MAX_SIZE_OF_HEADER)
		return 1;

	/* Check CRC before looking at remaining fields */
	if (HeaderCrc(h) != h->header_crc32)
		return 1;

	/* Reserved fields must be zero. */
	if (h->reserved_zero)
		return 1;

	/* Could check that padding is zero, but that doesn't matter to us. */

	/*
	 * If entry size is different than our struct, we won't be able to
	 * parse it.  Technically, any size 2^N where N>=7 is valid.
	 */
	if (h->size_of_entry != sizeof(GptEntry))
		return 1;
	if ((h->number_of_entries < MIN_NUMBER_OF_ENTRIES) ||
	    (h->number_of_entries > MAX_NUMBER_OF_ENTRIES) ||
	    (h->number_of_entries * h->size_of_entry != TOTAL_ENTRIES_SIZE))
		return 1;

	/*
	 * Check locations for the header and its entries.  The primary
	 * immediately follows the PMBR, and is followed by its entries.  The
	 * secondary is at the end of the drive, preceded by its entries.
	 */
	if (is_secondary) {
		if (h->my_lba != drive_sectors - 1)
			return 1;
		if (h->entries_lba != h->my_lba - GPT_ENTRIES_SECTORS)
			return 1;
	} else {
		if (h->my_lba != 1)
			return 1;
		if (h->entries_lba != h->my_lba + 1)
			return 1;
	}

	/*
	 * FirstUsableLBA must be after the end of the primary GPT table array.
	 * LastUsableLBA must be before the start of the secondary GPT table
	 * array.  FirstUsableLBA <= LastUsableLBA.
	 */
	if (h->first_usable_lba < 2 + GPT_ENTRIES_SECTORS)
		return 1;
	if (h->last_usable_lba >= drive_sectors - 1 - GPT_ENTRIES_SECTORS)
		return 1;
	if (h->first_usable_lba > h->last_usable_lba)
		return 1;

	/* Success */
	return 0;
}

int IsUnusedEntry(const GptEntry *e)
{
	static Guid zero = {{{0, 0, 0, 0, 0, {0, 0, 0, 0, 0, 0}}}};
	return !Memcmp(&zero, (const uint8_t*)(&e->type), sizeof(zero));
}

int IsKernelEntry(const GptEntry *e)
{
	static Guid chromeos_kernel = GPT_ENT_TYPE_CHROMEOS_KERNEL;
	return !Memcmp(&e->type, &chromeos_kernel, sizeof(Guid));
}

int CheckEntries(GptEntry *entries, GptHeader *h)
{
	GptEntry *entry;
	uint32_t crc32;
	uint32_t i;

	/* Check CRC before examining entries. */
	crc32 = Crc32((const uint8_t *)entries,
		      h->size_of_entry * h->number_of_entries);
	if (crc32 != h->entries_crc32)
		return GPT_ERROR_CRC_CORRUPTED;

	/* Check all entries. */
	for (i = 0, entry = entries; i < h->number_of_entries; i++, entry++) {
		GptEntry *e2;
		uint32_t i2;

		if (IsUnusedEntry(entry))
			continue;

		/* Entry must be in valid region. */
		if ((entry->starting_lba < h->first_usable_lba) ||
		    (entry->ending_lba > h->last_usable_lba) ||
		    (entry->ending_lba < entry->starting_lba))
			return GPT_ERROR_OUT_OF_REGION;

		/* Entry must not overlap other entries. */
		for (i2 = 0, e2 = entries; i2 < h->number_of_entries;
		     i2++, e2++) {
			if (i2 == i || IsUnusedEntry(e2))
				continue;

			if ((entry->starting_lba >= e2->starting_lba) &&
			    (entry->starting_lba <= e2->ending_lba))
				return GPT_ERROR_START_LBA_OVERLAP;
			if ((entry->ending_lba >= e2->starting_lba) &&
			    (entry->ending_lba <= e2->ending_lba))
				return GPT_ERROR_END_LBA_OVERLAP;

			/* UniqueGuid field must be unique. */
			if (0 == Memcmp(&entry->unique, &e2->unique,
					sizeof(Guid)))
				return GPT_ERROR_DUP_GUID;
		}
	}

	/* Success */
	return 0;
}

int HeaderFieldsSame(GptHeader *h1, GptHeader *h2)
{
	if (Memcmp(h1->signature, h2->signature, sizeof(h1->signature)))
		return 1;
	if (h1->revision != h2->revision)
		return 1;
	if (h1->size != h2->size)
		return 1;
	if (h1->reserved_zero != h2->reserved_zero)
		return 1;
	if (h1->first_usable_lba != h2->first_usable_lba)
		return 1;
	if (h1->last_usable_lba != h2->last_usable_lba)
		return 1;
	if (Memcmp(&h1->disk_uuid, &h2->disk_uuid, sizeof(Guid)))
		return 1;
	if (h1->number_of_entries != h2->number_of_entries)
		return 1;
	if (h1->size_of_entry != h2->size_of_entry)
		return 1;
	if (h1->entries_crc32 != h2->entries_crc32)
		return 1;

	return 0;
}

int GptSanityCheck(GptData *gpt)
{
	int retval;
	GptHeader *header1 = (GptHeader *)(gpt->primary_header);
	GptHeader *header2 = (GptHeader *)(gpt->secondary_header);
	GptEntry *entries1 = (GptEntry *)(gpt->primary_entries);
	GptEntry *entries2 = (GptEntry *)(gpt->secondary_entries);
	GptHeader *goodhdr = NULL;

	gpt->valid_headers = 0;
	gpt->valid_entries = 0;

	retval = CheckParameters(gpt);
	if (retval != GPT_SUCCESS)
		return retval;

	/* Check both headers; we need at least one valid header. */
	if (0 == CheckHeader(header1, 0, gpt->drive_sectors)) {
		gpt->valid_headers |= MASK_PRIMARY;
		goodhdr = header1;
	}
	if (0 == CheckHeader(header2, 1, gpt->drive_sectors)) {
		gpt->valid_headers |= MASK_SECONDARY;
		if (!goodhdr)
			goodhdr = header2;
	}

	if (!gpt->valid_headers)
		return GPT_ERROR_INVALID_HEADERS;

	/*
	 * Check if entries are valid.
	 *
	 * Note that we use the same header in both checks.  This way we'll
	 * catch the case where (header1,entries1) and (header2,entries2) are
	 * both valid, but (entries1 != entries2).
	 */
	if (0 == CheckEntries(entries1, goodhdr))
		gpt->valid_entries |= MASK_PRIMARY;
	if (0 == CheckEntries(entries2, goodhdr))
		gpt->valid_entries |= MASK_SECONDARY;

	/*
	 * If both headers are good but neither entries were good, check the
	 * entries with the secondary header.
	 */
	if (MASK_BOTH == gpt->valid_headers && !gpt->valid_entries) {
		if (0 == CheckEntries(entries1, header2))
			gpt->valid_entries |= MASK_PRIMARY;
		if (0 == CheckEntries(entries2, header2))
			gpt->valid_entries |= MASK_SECONDARY;
		if (gpt->valid_entries) {
			/*
			 * Sure enough, header2 had a good CRC for one of the
			 * entries.  Mark header1 invalid, so we'll update its
			 * entries CRC.
			 */
			gpt->valid_headers &= ~MASK_PRIMARY;
			goodhdr = header2;
		}
	}

	if (!gpt->valid_entries)
		return GPT_ERROR_INVALID_ENTRIES;

	/*
	 * Now that we've determined which header contains a good CRC for
	 * the entries, make sure the headers are otherwise identical.
	 */
	if (MASK_BOTH == gpt->valid_headers &&
	    0 != HeaderFieldsSame(header1, header2))
		gpt->valid_headers &= ~MASK_SECONDARY;

	return GPT_SUCCESS;
}

static int GptRecomputeSize(GptData *gpt)
{
	GptHeader backup, *header;
	uint64_t alt_lba, alt_entries_lba, last_usable_lba;
	uint32_t was_valid;

	alt_lba = gpt->drive_sectors - 1;
	alt_entries_lba = alt_lba - GPT_ENTRIES_SECTORS;
	last_usable_lba = alt_entries_lba - 1;

	/* If the preferred header matches the above values based on the
	 * disk size then all is good and quit. Otherwise try to update. */
	if (MASK_PRIMARY & gpt->valid_headers) {
		header = (GptHeader *)(gpt->primary_header);
		if (header->alternate_lba == alt_lba &&
		    header->last_usable_lba == last_usable_lba)
			return GPT_SUCCESS;

		Memcpy(&backup, header, sizeof(GptHeader));
		header->alternate_lba = alt_lba;
		header->last_usable_lba = last_usable_lba;
		header->header_crc32 = HeaderCrc(header);
		was_valid = MASK_PRIMARY;
	}
	else if (MASK_SECONDARY & gpt->valid_headers) {
		header = (GptHeader *)(gpt->secondary_header);
		if (header->my_lba == alt_lba &&
		    header->entries_lba == alt_entries_lba &&
		    header->last_usable_lba == last_usable_lba)
			return GPT_SUCCESS;

		Memcpy(&backup, header, sizeof(GptHeader));
		header->my_lba = alt_lba;
		header->entries_lba = alt_entries_lba;
		header->last_usable_lba = last_usable_lba;
		header->header_crc32 = HeaderCrc(header);
		was_valid = MASK_SECONDARY;
	}
	else {
		return GPT_ERROR_INVALID_HEADERS;
	}

	/* Hopefully the header we just updated is valid and not the other.
	 * If that isn't give up and clean up our mess. */
	if (GptSanityCheck(gpt) != GPT_SUCCESS ||
	    gpt->valid_headers != was_valid) {
		Memcpy(header, &backup, sizeof(GptHeader));
		GptSanityCheck(gpt);
		return GPT_ERROR_INVALID_HEADERS;
	}

	/* Write out secondary no matter what since its location changed. */
	gpt->modified |= GPT_MODIFIED_HEADER2 | GPT_MODIFIED_ENTRIES2;
	if (was_valid == MASK_PRIMARY)
		gpt->modified |= GPT_MODIFIED_HEADER1;

	return GPT_SUCCESS;
}

void GptRepair(GptData *gpt)
{
	GptHeader *header1 = (GptHeader *)(gpt->primary_header);
	GptHeader *header2 = (GptHeader *)(gpt->secondary_header);
	GptEntry *entries1 = (GptEntry *)(gpt->primary_entries);
	GptEntry *entries2 = (GptEntry *)(gpt->secondary_entries);
	int entries_size;

	/* Need at least one good header and one good set of entries. */
	if (MASK_NONE == gpt->valid_headers || MASK_NONE == gpt->valid_entries)
		return;

	/* Update whichever header is valid based on disk size. */
	if (GptRecomputeSize(gpt) != GPT_SUCCESS)
		return;

	/* Repair headers if necessary */
	if (MASK_PRIMARY == gpt->valid_headers) {
		/* Primary is good, secondary is bad */
		Memcpy(header2, header1, sizeof(GptHeader));
		header2->my_lba = gpt->drive_sectors - 1;
		header2->alternate_lba = 1;
		header2->entries_lba = header2->my_lba - GPT_ENTRIES_SECTORS;
		header2->header_crc32 = HeaderCrc(header2);
		gpt->modified |= GPT_MODIFIED_HEADER2;
	}
	else if (MASK_SECONDARY == gpt->valid_headers) {
		/* Secondary is good, primary is bad */
		Memcpy(header1, header2, sizeof(GptHeader));
		header1->my_lba = 1;
		header1->alternate_lba = gpt->drive_sectors - 1;
		header1->entries_lba = header1->my_lba + 1;
		header1->header_crc32 = HeaderCrc(header1);
		gpt->modified |= GPT_MODIFIED_HEADER1;
	}
	gpt->valid_headers = MASK_BOTH;

	/* Repair entries if necessary */
	entries_size = header1->size_of_entry * header1->number_of_entries;
	if (MASK_PRIMARY == gpt->valid_entries) {
		/* Primary is good, secondary is bad */
		Memcpy(entries2, entries1, entries_size);
		gpt->modified |= GPT_MODIFIED_ENTRIES2;
	}
	else if (MASK_SECONDARY == gpt->valid_entries) {
		/* Secondary is good, primary is bad */
		Memcpy(entries1, entries2, entries_size);
		gpt->modified |= GPT_MODIFIED_ENTRIES1;
	}
	gpt->valid_entries = MASK_BOTH;
}

int GetEntryLegacyBootable(const GptEntry *e)
{
	return !!(e->attrs.whole & CGPT_ATTRIBUTE_LEGACY_BOOTABLE);
}

int GetEntrySuccessful(const GptEntry *e)
{
	return (e->attrs.fields.gpt_att & CGPT_ATTRIBUTE_SUCCESSFUL_MASK) >>
		CGPT_ATTRIBUTE_SUCCESSFUL_OFFSET;
}

int GetEntryPriority(const GptEntry *e)
{
	return (e->attrs.fields.gpt_att & CGPT_ATTRIBUTE_PRIORITY_MASK) >>
		CGPT_ATTRIBUTE_PRIORITY_OFFSET;
}

int GetEntryTries(const GptEntry *e)
{
	return (e->attrs.fields.gpt_att & CGPT_ATTRIBUTE_TRIES_MASK) >>
		CGPT_ATTRIBUTE_TRIES_OFFSET;
}

void SetEntryLegacyBootable(GptEntry *e, int bootable)
{
	if (bootable)
		e->attrs.whole |= CGPT_ATTRIBUTE_LEGACY_BOOTABLE;
	else
		e->attrs.whole &= ~CGPT_ATTRIBUTE_LEGACY_BOOTABLE;
}

void SetEntrySuccessful(GptEntry *e, int successful)
{
	if (successful)
		e->attrs.fields.gpt_att |= CGPT_ATTRIBUTE_SUCCESSFUL_MASK;
	else
		e->attrs.fields.gpt_att &= ~CGPT_ATTRIBUTE_SUCCESSFUL_MASK;
}

void SetEntryPriority(GptEntry *e, int priority)
{
	e->attrs.fields.gpt_att &= ~CGPT_ATTRIBUTE_PRIORITY_MASK;
	e->attrs.fields.gpt_att |=
		(priority << CGPT_ATTRIBUTE_PRIORITY_OFFSET) &
		CGPT_ATTRIBUTE_PRIORITY_MASK;
}

void SetEntryTries(GptEntry *e, int tries)
{
	e->attrs.fields.gpt_att &= ~CGPT_ATTRIBUTE_TRIES_MASK;
	e->attrs.fields.gpt_att |= (tries << CGPT_ATTRIBUTE_TRIES_OFFSET) &
		CGPT_ATTRIBUTE_TRIES_MASK;
}

void GetCurrentKernelUniqueGuid(GptData *gpt, void *dest)
{
	GptEntry *entries = (GptEntry *)gpt->primary_entries;
	GptEntry *e = entries + gpt->current_kernel;
	Memcpy(dest, &e->unique, sizeof(Guid));
}

void GptModified(GptData *gpt) {
	GptHeader *header = (GptHeader *)gpt->primary_header;

	/* Update the CRCs */
	header->entries_crc32 = Crc32(gpt->primary_entries,
				      header->size_of_entry *
				      header->number_of_entries);
	header->header_crc32 = HeaderCrc(header);
	gpt->modified |= GPT_MODIFIED_HEADER1 | GPT_MODIFIED_ENTRIES1;

	/*
	 * Use the repair function to update the other copy of the GPT.  This
	 * is a tad inefficient, but is much faster than the disk I/O to update
	 * the GPT on disk so it doesn't matter.
	 */
	gpt->valid_headers = MASK_PRIMARY;
	gpt->valid_entries = MASK_PRIMARY;
	GptRepair(gpt);
}


const char *GptErrorText(int error_code)
{
	switch(error_code) {
	case GPT_SUCCESS:
		return "none";

	case GPT_ERROR_NO_VALID_KERNEL:
		return "Invalid kernel";

	case GPT_ERROR_INVALID_HEADERS:
		return "Invalid headers";

	case GPT_ERROR_INVALID_ENTRIES:
		return "Invalid entries";

	case GPT_ERROR_INVALID_SECTOR_SIZE:
		return "Invalid sector size";

	case GPT_ERROR_INVALID_SECTOR_NUMBER:
		return "Invalid sector number";

	case GPT_ERROR_INVALID_UPDATE_TYPE:
		return "Invalid update type";

	case GPT_ERROR_CRC_CORRUPTED:
		return "Entries' crc corrupted";

	case GPT_ERROR_OUT_OF_REGION:
		return "Entry outside of valid region";

	case GPT_ERROR_START_LBA_OVERLAP:
		return "Starting LBA overlaps";

	case GPT_ERROR_END_LBA_OVERLAP:
		return "Ending LBA overlaps";

	case GPT_ERROR_DUP_GUID:
		return "Duplicated GUID";

	case GPT_ERROR_INVALID_FLASH_GEOMETRY:
		return "Invalid flash geometry";

	case GPT_ERROR_NO_SUCH_ENTRY:
		return "No entry found";

	default:
		break;
	};
	return "Unknown";
}
