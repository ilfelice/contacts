/*
 * Contacts - VCardImporter
 *
 * Distributed under the terms of the MIT License.
 * Copyright 2026 Il Felice.
 *
 * Imports vCard (.vcf) files into Haiku People files
 * (application/x-person with META:* attributes) in ~/people.
 *
 * Supported subset (see ContactsWindow design notes):
 *   - vCard 2.1 / 3.0 / 4.0, multi-contact files, line unfolding,
 *     UTF-8, quoted-printable (2.1-era exports)
 *   - FN (fallback: N) -> META:name, NICKNAME -> META:nickname,
 *     first EMAIL (PREF preferred) -> META:email,
 *     TEL by TYPE -> META:mphone/hphone/wphone/fax,
 *     ORG -> META:company, first ADR -> META:address/city/state/
 *     zip/country, URL -> META:url,
 *     CATEGORIES -> META:group
 *   - PHOTO, NOTE, BDAY and everything else: skipped
 *
 * Duplicates are matched by META:email (query on the destination
 * volume) and skipped.
 */

#ifndef VCARD_IMPORTER_H
#define VCARD_IMPORTER_H

#include <Entry.h>
#include <Path.h>
#include <String.h>
#include <SupportDefs.h>


class VCardImporter {
public:
	// Import every vCard in the given .vcf file. Contacts are written
	// as People files into ~/people (created if missing). Returns
	// B_OK when the file was read and parsed, even if some or all
	// contacts were skipped; *outImported and *outSkipped receive the
	// counts. On read/parse failure returns an error and sets
	// *outError to a human-readable message.
	static status_t		ImportFile(const entry_ref& ref,
							int32* outImported, int32* outSkipped,
							BString* outError);

	// Path of the import log (skipped entries with reasons, one dated
	// section per imported file), for display in the completion alert.
	static status_t		LogPath(BPath& path);
};


#endif // VCARD_IMPORTER_H
