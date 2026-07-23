/*
 * Contacts - VCardImporter
 *
 * Distributed under the terms of the MIT License.
 * Copyright 2026 Il Felice.
 */

#include "VCardImporter.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Query.h>
#include <Volume.h>
#include <fs_index.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <set>
#include <vector>


// One parsed contact (only the fields we map).
struct VCardContact {
	BString name;
	BString nickname;
	BString email;
	BString cellphone;
	BString hphone;
	BString wphone;
	BString fax;
	BString company;
	BString address;
	BString city;
	BString state;
	BString zip;
	BString country;
	BString homepage;
	BString group;

	bool IsEmpty() const
	{
		return name.Length() == 0 && email.Length() == 0;
	}
};


// --- Text helpers ---


// Decode quoted-printable in place ("=E6=97=A5" -> bytes; "=\n" soft
// breaks were already joined during unfolding).
static void
DecodeQuotedPrintable(BString& text)
{
	BString out;
	int32 length = text.Length();
	for (int32 i = 0; i < length; i++) {
		char c = text.ByteAt(i);
		if (c == '=' && i + 2 < length
			&& isxdigit(text.ByteAt(i + 1))
			&& isxdigit(text.ByteAt(i + 2))) {
			char hex[3] = { text.ByteAt(i + 1), text.ByteAt(i + 2), 0 };
			out << (char)strtol(hex, NULL, 16);
			i += 2;
		} else
			out << c;
	}
	text = out;
}


// Unescape vCard value escapes: \n \N \\ \; \,
static void
UnescapeValue(BString& text)
{
	BString out;
	int32 length = text.Length();
	for (int32 i = 0; i < length; i++) {
		char c = text.ByteAt(i);
		if (c == '\\' && i + 1 < length) {
			char next = text.ByteAt(i + 1);
			if (next == 'n' || next == 'N') {
				out << '\n';
				i++;
				continue;
			}
			if (next == '\\' || next == ';' || next == ',') {
				out << next;
				i++;
				continue;
			}
		}
		out << c;
	}
	out.Trim();
	text = out;
}


// Split a ';'-separated component list, honoring '\;' escapes.
static void
SplitComponents(const BString& value, std::vector<BString>& outParts)
{
	BString current;
	int32 length = value.Length();
	for (int32 i = 0; i < length; i++) {
		char c = value.ByteAt(i);
		if (c == '\\' && i + 1 < length) {
			current << c << value.ByteAt(i + 1);
			i++;
			continue;
		}
		if (c == ';') {
			outParts.push_back(current);
			current = "";
			continue;
		}
		current << c;
	}
	outParts.push_back(current);
}


// --- Parsing ---


// One "NAME;PARAM;PARAM:value" content line, already unfolded.
struct VCardLine {
	BString name;      // uppercased, group prefix stripped
	BString params;    // uppercased, raw ";"-joined parameter text
	BString value;     // raw value (not yet unescaped)

	bool HasParam(const char* token) const
	{
		return params.FindFirst(token) >= 0;
	}
};


static bool
ParseLine(const BString& line, VCardLine& out)
{
	int32 colon = line.FindFirst(':');
	if (colon <= 0)
		return false;

	BString head(line.String(), colon);
	out.value.SetTo(line.String() + colon + 1);

	int32 semi = head.FindFirst(';');
	if (semi >= 0) {
		head.CopyInto(out.name, 0, semi);
		head.CopyInto(out.params, semi + 1, head.Length() - semi - 1);
	} else {
		out.name = head;
		out.params = "";
	}

	// Strip a group prefix ("item1.EMAIL" -> "EMAIL", iCloud exports).
	int32 dot = out.name.FindFirst('.');
	if (dot >= 0)
		out.name.Remove(0, dot + 1);

	out.name.ToUpper();
	out.params.ToUpper();
	return true;
}


static void
ApplyLine(const VCardLine& line, VCardContact& contact)
{
	BString value = line.value;
	if (line.HasParam("QUOTED-PRINTABLE"))
		DecodeQuotedPrintable(value);

	if (line.name == "FN") {
		UnescapeValue(value);
		if (value.Length() > 0)
			contact.name = value;
	} else if (line.name == "N") {
		// Fallback only: FN wins when present.
		if (contact.name.Length() == 0) {
			std::vector<BString> parts;
			SplitComponents(value, parts);
			BString family = parts.size() > 0 ? parts[0] : BString();
			BString given = parts.size() > 1 ? parts[1] : BString();
			UnescapeValue(family);
			UnescapeValue(given);
			BString full = given;
			if (given.Length() > 0 && family.Length() > 0)
				full << " ";
			full << family;
			if (full.Length() > 0)
				contact.name = full;
		}
	} else if (line.name == "NICKNAME") {
		UnescapeValue(value);
		contact.nickname = value;
	} else if (line.name == "EMAIL") {
		UnescapeValue(value);
		// First email wins unless a later one is marked preferred.
		if (contact.email.Length() == 0 || line.HasParam("PREF"))
			contact.email = value;
	} else if (line.name == "TEL") {
		UnescapeValue(value);
		if (value.Length() == 0)
			return;
		if (line.HasParam("CELL") || line.HasParam("MOBILE")) {
			if (contact.cellphone.Length() == 0)
				contact.cellphone = value;
		} else if (line.HasParam("FAX")) {
			if (contact.fax.Length() == 0)
				contact.fax = value;
		} else if (line.HasParam("WORK")) {
			if (contact.wphone.Length() == 0)
				contact.wphone = value;
		} else {
			// HOME and untyped both land in the home slot.
			if (contact.hphone.Length() == 0)
				contact.hphone = value;
		}
	} else if (line.name == "ORG") {
		std::vector<BString> parts;
		SplitComponents(value, parts);
		if (!parts.empty()) {
			UnescapeValue(parts[0]);
			contact.company = parts[0];
		}
	} else if (line.name == "ADR") {
		// Components: pobox;extended;street;locality;region;postal;
		// country. First ADR wins.
		if (contact.address.Length() > 0 || contact.city.Length() > 0)
			return;
		std::vector<BString> parts;
		SplitComponents(value, parts);
		while (parts.size() < 7)
			parts.push_back(BString());
		UnescapeValue(parts[2]);
		UnescapeValue(parts[3]);
		UnescapeValue(parts[4]);
		UnescapeValue(parts[5]);
		UnescapeValue(parts[6]);
		contact.address = parts[2];
		contact.city = parts[3];
		contact.state = parts[4];
		contact.zip = parts[5];
		contact.country = parts[6];
	} else if (line.name == "URL") {
		UnescapeValue(value);
		if (contact.homepage.Length() == 0)
			contact.homepage = value;
	} else if (line.name == "CATEGORIES") {
		// vCard categories (Gmail exports contact labels here) map to
		// People's Group field. Multiple values arrive comma- (3.0+)
		// or semicolon- (2.1) separated; People uses commas.
		UnescapeValue(value);
		value.ReplaceAll(";", ", ");
		contact.group = value;
	}
	// Everything else (PHOTO, NOTE, BDAY, ...): skipped by design.
}


// Read the whole file, normalize line ends, unfold continuations.
// Folding: a line starting with space/tab continues the previous line
// (RFC 6350 §3.2). Quoted-printable soft breaks: a QP line ending in
// '=' continues on the next line (vCard 2.1) — joined here too so the
// decoder sees one logical value.
static status_t
ReadUnfolded(const entry_ref& ref, std::vector<BString>& outLines,
	BString* outError)
{
	BFile file(&ref, B_READ_ONLY);
	if (file.InitCheck() != B_OK) {
		*outError = "The file could not be opened.";
		return file.InitCheck();
	}

	off_t size = 0;
	file.GetSize(&size);
	if (size <= 0 || size > 32 * 1024 * 1024) {
		*outError = "The file is empty or unreasonably large.";
		return B_BAD_VALUE;
	}

	BString text;
	char* buffer = text.LockBuffer((int32)size + 1);
	ssize_t read = file.Read(buffer, (size_t)size);
	if (read < 0) {
		text.UnlockBuffer(0);
		*outError = "The file could not be read.";
		return (status_t)read;
	}
	buffer[read] = '\0';
	text.UnlockBuffer((int32)read);

	text.ReplaceAll("\r\n", "\n");
	text.ReplaceAll("\r", "\n");

	// Split into raw lines.
	std::vector<BString> raw;
	int32 start = 0;
	while (start <= text.Length()) {
		int32 end = text.FindFirst('\n', start);
		if (end < 0)
			end = text.Length();
		BString line(text.String() + start, end - start);
		raw.push_back(line);
		start = end + 1;
	}

	// Unfold.
	for (size_t i = 0; i < raw.size(); i++) {
		const BString& line = raw[i];
		if (line.Length() == 0)
			continue;
		char first = line.ByteAt(0);
		if ((first == ' ' || first == '\t') && !outLines.empty()) {
			// Continuation: append without the leading whitespace.
			outLines.back().Append(line.String() + 1,
				line.Length() - 1);
			continue;
		}
		if (!outLines.empty() && outLines.back().Length() > 0
			&& outLines.back().ByteAt(outLines.back().Length() - 1)
				== '='
			&& outLines.back().IFindFirst("QUOTED-PRINTABLE") >= 0) {
			// QP soft break: strip the trailing '=' and join.
			outLines.back().Truncate(outLines.back().Length() - 1);
			outLines.back().Append(line);
			continue;
		}
		outLines.push_back(line);
	}

	return B_OK;
}


// --- Writing ---


static void
WriteAttrIfSet(BNode& node, const char* attribute, const BString& value)
{
	if (value.Length() > 0)
		node.WriteAttrString(attribute, &value);
}


static void
SanitizeFileName(BString& name)
{
	name.ReplaceAll("/", "-");
	name.Trim();
	if (name.Length() > B_FILE_NAME_LENGTH - 8)
		name.Truncate(B_FILE_NAME_LENGTH - 8);
	if (name.Length() == 0)
		name = "Imported contact";
}


// Collect every existing value of the given META attribute on the
// destination volume, lowercased — the duplicate checks are
// deliberately case-insensitive (BFS string equality is not, and
// exporters disagree about casing). One query per attribute per
// import run; the sets also catch duplicates within the imported
// file itself.
static void
CollectExistingValues(dev_t device, const char* attribute,
	std::set<BString>& outValues)
{
	BVolume volume(device);
	BQuery query;
	query.SetVolume(&volume);
	BString predicate;
	predicate << attribute << "=\"**\"";
	query.SetPredicate(predicate.String());
	if (query.Fetch() != B_OK)
		return;

	entry_ref ref;
	while (query.GetNextRef(&ref) == B_OK) {
		BNode node(&ref);
		BString value;
		if (node.InitCheck() == B_OK
			&& node.ReadAttrString(attribute, &value) == B_OK
			&& value.Length() > 0) {
			value.ToLower();
			outValues.insert(value);
		}
	}
}


static status_t
WriteContact(const VCardContact& contact, BDirectory& peopleDir)
{
	BString fileName = contact.name.Length() > 0
		? contact.name : contact.email;
	SanitizeFileName(fileName);

	// Collision-safe creation (B_FAIL_IF_EXISTS + numeric suffixes).
	BFile file;
	BString tryName = fileName;
	status_t status = B_FILE_EXISTS;
	for (int n = 1; n <= 99 && status == B_FILE_EXISTS; n++) {
		if (n > 1) {
			tryName = fileName;
			tryName << " " << n;
		}
		status = peopleDir.CreateFile(tryName.String(), &file, true);
	}
	if (status != B_OK)
		return status;

	BNodeInfo nodeInfo(&file);
	nodeInfo.SetType("application/x-person");

	WriteAttrIfSet(file, "META:name", contact.name);
	WriteAttrIfSet(file, "META:nickname", contact.nickname);
	WriteAttrIfSet(file, "META:email", contact.email);
	WriteAttrIfSet(file, "META:mphone", contact.cellphone);
	WriteAttrIfSet(file, "META:hphone", contact.hphone);
	WriteAttrIfSet(file, "META:wphone", contact.wphone);
	WriteAttrIfSet(file, "META:fax", contact.fax);
	WriteAttrIfSet(file, "META:company", contact.company);
	WriteAttrIfSet(file, "META:address", contact.address);
	WriteAttrIfSet(file, "META:city", contact.city);
	WriteAttrIfSet(file, "META:state", contact.state);
	WriteAttrIfSet(file, "META:zip", contact.zip);
	WriteAttrIfSet(file, "META:country", contact.country);
	WriteAttrIfSet(file, "META:url", contact.homepage);
	WriteAttrIfSet(file, "META:group", contact.group);

	file.Sync();
	return B_OK;
}


// --- Import log ---


/*static*/ status_t
VCardImporter::LogPath(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append("Contacts");
	create_directory(path.Path(), 0755);
	path.Append("contacts_import.log");
	return B_OK;
}


static void
AppendToLog(BFile& log, const BString& line)
{
	if (log.InitCheck() == B_OK)
		log.Write(line.String(), line.Length());
}


static BString
DescribeContact(const VCardContact& contact)
{
	BString text;
	text << (contact.name.Length() > 0
		? contact.name.String() : "(no name)");
	text << " <" << (contact.email.Length() > 0
		? contact.email.String() : "no email") << ">";
	return text;
}


// --- Public entry point ---


/*static*/ status_t
VCardImporter::ImportFile(const entry_ref& ref, int32* outImported,
	int32* outSkipped, BString* outError)
{
	*outImported = 0;
	*outSkipped = 0;
	outError->SetTo("");

	std::vector<BString> lines;
	status_t status = ReadUnfolded(ref, lines, outError);
	if (status != B_OK)
		return status;

	// Destination: ~/people (People app convention), created if
	// missing.
	BPath peoplePath;
	status = find_directory(B_USER_DIRECTORY, &peoplePath);
	if (status != B_OK) {
		*outError = "The home directory could not be located.";
		return status;
	}
	peoplePath.Append("people");
	create_directory(peoplePath.Path(), 0755);
	BDirectory peopleDir(peoplePath.Path());
	if (peopleDir.InitCheck() != B_OK) {
		*outError = "The people folder could not be created.";
		return peopleDir.InitCheck();
	}

	// Make sure the destination volume carries the People indexes —
	// attributes written before an index exists stay invisible to
	// queries (same lesson as the mail store). ~/people is on the
	// boot volume, which ships indexed, so these are no-ops there;
	// this is insurance. fs_create_index is idempotent.
	node_ref dirNode;
	peopleDir.GetNodeRef(&dirNode);
	fs_create_index(dirNode.device, "META:name", B_STRING_TYPE, 0);
	fs_create_index(dirNode.device, "META:email", B_STRING_TYPE, 0);
	fs_create_index(dirNode.device, "META:nickname", B_STRING_TYPE, 0);

	std::set<BString> existingEmails;
	CollectExistingValues(dirNode.device, "META:email", existingEmails);
	std::set<BString> sessionEmails;
	// Fallback identity for contacts WITHOUT an email address (they
	// would otherwise re-import on every run): the name,
	// case-insensitive. Only consulted when the email key is empty —
	// email remains the stronger identity when present.
	std::set<BString> existingNames;
	CollectExistingValues(dirNode.device, "META:name", existingNames);
	std::set<BString> sessionNames;

	BFile log;
	BPath logPath;
	if (LogPath(logPath) == B_OK) {
		log.SetTo(logPath.Path(),
			B_WRITE_ONLY | B_CREATE_FILE | B_OPEN_AT_END);
		if (log.InitCheck() == B_OK) {
			// The log is UTF-8; a proper type helps editors detect it.
			BNodeInfo(&log).SetType("text/plain");
		}
	}
	time_t now = time(NULL);
	char stamp[64];
	strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S",
		localtime(&now));
	BString header;
	header << "=== Import " << stamp << ": " << ref.name << " ===\n";
	AppendToLog(log, header);

	// Parse blocks and write contacts.
	bool inCard = false;
	bool sawCard = false;
	VCardContact contact;
	for (size_t i = 0; i < lines.size(); i++) {
		VCardLine line;
		if (!ParseLine(lines[i], line))
			continue;

		if (line.name == "BEGIN" && line.value.ICompare("VCARD") == 0) {
			inCard = true;
			sawCard = true;
			contact = VCardContact();
			continue;
		}
		if (line.name == "END" && line.value.ICompare("VCARD") == 0) {
			if (inCard && !contact.IsEmpty()) {
				BString emailKey = contact.email;
				emailKey.ToLower();
				BString nameKey = contact.name;
				nameKey.ToLower();
				bool byName = emailKey.Length() == 0
					&& nameKey.Length() > 0;
				BString logLine;
				if (emailKey.Length() > 0
					&& sessionEmails.find(emailKey)
						!= sessionEmails.end()) {
					(*outSkipped)++;
					logLine << "SKIPPED (duplicate within this file): "
						<< DescribeContact(contact) << "\n";
				} else if (emailKey.Length() > 0
					&& existingEmails.find(emailKey)
						!= existingEmails.end()) {
					(*outSkipped)++;
					logLine << "SKIPPED (already in People): "
						<< DescribeContact(contact) << "\n";
				} else if (byName
					&& sessionNames.find(nameKey)
						!= sessionNames.end()) {
					(*outSkipped)++;
					logLine << "SKIPPED (duplicate name within this "
						"file, no email): "
						<< DescribeContact(contact) << "\n";
				} else if (byName
					&& existingNames.find(nameKey)
						!= existingNames.end()) {
					(*outSkipped)++;
					logLine << "SKIPPED (name already in People, "
						"no email): "
						<< DescribeContact(contact) << "\n";
				} else if (WriteContact(contact, peopleDir) == B_OK) {
					(*outImported)++;
					if (emailKey.Length() > 0) {
						existingEmails.insert(emailKey);
						sessionEmails.insert(emailKey);
					}
					if (nameKey.Length() > 0) {
						existingNames.insert(nameKey);
						sessionNames.insert(nameKey);
					}
				} else {
					(*outSkipped)++;
					logLine << "SKIPPED (could not write file): "
						<< DescribeContact(contact) << "\n";
				}
				if (logLine.Length() > 0)
					AppendToLog(log, logLine);
			}
			inCard = false;
			continue;
		}
		if (inCard)
			ApplyLine(line, contact);
	}

	BString summary;
	summary << "Imported " << *outImported << ", skipped "
		<< *outSkipped << "\n\n";
	AppendToLog(log, summary);

	if (!sawCard) {
		*outError = "No vCard entries were found in the file.";
		return B_BAD_VALUE;
	}

	return B_OK;
}
