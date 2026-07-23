/*
 * Contacts - ContactsWindow
 *
 * Distributed under the terms of the MIT License.
 * Copyright 2026 Il Felice.
 */

#include "ContactsWindow.h"
#include "VCardImporter.h"

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Directory.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MessageFilter.h>
#include <MenuItem.h>
#include <NodeInfo.h>
#include <NodeMonitor.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Query.h>
#include <Roster.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include <stdio.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ContactsWindow"


static const char* kPeopleAppSignature = "application/x-vnd.Be-PEPL";

static const uint32 kMsgAddContact = 'ctad';
static const uint32 kMsgEditContact = 'cted';
static const uint32 kMsgRemoveContact = 'ctrm';
static const uint32 kMsgImportVCard = 'ctim';
static const uint32 kMsgFilterChanged = 'ctfl';
static const uint32 kMsgSelectionChanged = 'ctsl';
static const uint32 kMsgContactInvoked = 'ctiv';


// A list row bound to one People file.
class ContactRow : public BRow {
public:
	ContactRow(const node_ref& nodeRef, const entry_ref& ref)
		:
		fNodeRef(nodeRef),
		fRef(ref)
	{
		for (int32 i = 0; i < 5; i++)
			SetField(new BStringField(""), i);
		Reload();
	}

	// Re-read the displayed attributes from the file.
	void Reload()
	{
		BNode node(&fRef);
		fName = "";
		fEmail = "";
		fNickname = "";
		fCompany = "";
		fPhone = "";
		fUrl = "";
		if (node.InitCheck() == B_OK) {
			node.ReadAttrString("META:name", &fName);
			node.ReadAttrString("META:email", &fEmail);
			node.ReadAttrString("META:nickname", &fNickname);
			node.ReadAttrString("META:company", &fCompany);
			// One phone column: first non-empty of mobile/home/work.
			node.ReadAttrString("META:mphone", &fPhone);
			if (fPhone.Length() == 0)
				node.ReadAttrString("META:hphone", &fPhone);
			if (fPhone.Length() == 0)
				node.ReadAttrString("META:wphone", &fPhone);
			node.ReadAttrString("META:url", &fUrl);
		}
		if (fName.Length() == 0)
			fName = fRef.name;

		((BStringField*)GetField(0))->SetString(fName.String());
		((BStringField*)GetField(1))->SetString(fEmail.String());
		((BStringField*)GetField(2))->SetString(fCompany.String());
		((BStringField*)GetField(3))->SetString(fPhone.String());
		((BStringField*)GetField(4))->SetString(fUrl.String());
	}

	bool MatchesFilter(const BString& filter) const
	{
		if (filter.Length() == 0)
			return true;
		return fName.IFindFirst(filter) >= 0
			|| fEmail.IFindFirst(filter) >= 0
			|| fNickname.IFindFirst(filter) >= 0;
	}

	const node_ref& NodeRef() const { return fNodeRef; }
	const entry_ref& Ref() const { return fRef; }
	void SetRef(const entry_ref& ref) { fRef = ref; }

private:
	node_ref	fNodeRef;
	entry_ref	fRef;
	BString		fName;
	BString		fEmail;
	BString		fNickname;
	BString		fCompany;
	BString		fPhone;
	BString		fUrl;
};


// Column list with a right-click menu toggling column visibility.
// Order and width changes are native (drag); everything is persisted
// by the window at close via SaveState().
class ContactListView : public BColumnListView {
public:
	ContactListView(const char* name)
		:
		BColumnListView(name, 0, B_FANCY_BORDER)
	{
	}

	virtual void MouseDown(BPoint where)
	{
		int32 buttons = 0;
		if (Window() != NULL && Window()->CurrentMessage() != NULL) {
			Window()->CurrentMessage()->FindInt32("buttons",
				&buttons);
		}
		if ((buttons & B_SECONDARY_MOUSE_BUTTON) == 0) {
			BColumnListView::MouseDown(where);
			return;
		}

		BPopUpMenu* menu = new BPopUpMenu("columns", false, false);
		for (int32 i = 0; i < CountColumns(); i++) {
			BColumn* column = ColumnAt(i);
			BString title;
			((BTitledColumn*)column)->GetColumnName(&title);
			BMenuItem* item = new BMenuItem(title.String(), NULL);
			item->SetMarked(column->IsVisible());
			// The Name column stays: hiding every column would leave
			// an unusable list.
			item->SetEnabled(i != 0);
			menu->AddItem(item);
		}
		BMenuItem* chosen = menu->Go(ConvertToScreen(where), false,
			true);
		if (chosen != NULL) {
			int32 index = menu->IndexOf(chosen);
			BColumn* column = ColumnAt(index);
			if (column != NULL)
				column->SetVisible(!column->IsVisible());
		}
		delete menu;
	}
};


// People files sitting in the Trash still match the query; hide them,
// mirroring what users expect from "removed". The trash directory is
// resolved per volume via find_directory — its on-disk name is NOT
// "Trash" (on Haiku it is the hidden volume-root "trash"), so no path
// string is assumed.
static bool
IsInTrash(const entry_ref& ref)
{
	BEntry entry(&ref);
	BPath path;
	if (entry.GetPath(&path) != B_OK || path.Path() == NULL)
		return false;

	BVolume volume(ref.device);
	BPath trashPath;
	if (find_directory(B_TRASH_DIRECTORY, &trashPath, false, &volume)
			!= B_OK || trashPath.Path() == NULL)
		return false;

	BString prefix(trashPath.Path());
	if (!prefix.EndsWith("/"))
		prefix << "/";
	return BString(path.Path()).StartsWith(prefix);
}


// Window-level keys: Delete removes the selected contacts — except
// while a text view has focus, where Delete must keep editing text
// (the filter field). (The integrated EmailViewsNeo version also
// closes on Escape; a standalone primary window should not.)
class KeyFilter : public BMessageFilter {
public:
	KeyFilter(BWindow* window)
		:
		BMessageFilter(B_ANY_DELIVERY, B_ANY_SOURCE, B_KEY_DOWN),
		fWindow(window)
	{
	}

	filter_result Filter(BMessage* message, BHandler** target)
	{
		const char* bytes = NULL;
		if (message->FindString("bytes", &bytes) != B_OK
			|| bytes == NULL)
			return B_DISPATCH_MESSAGE;

		if (bytes[0] == B_DELETE
			&& dynamic_cast<BTextView*>(fWindow->CurrentFocus())
				== NULL) {
			fWindow->PostMessage(kMsgRemoveContact);
			return B_SKIP_MESSAGE;
		}
		return B_DISPATCH_MESSAGE;
	}

private:
	BWindow* fWindow;
};


// Window state persistence (frame + column arrangement) in the app's
// own settings file. Keys are kept identical to the EmailViewsNeo
// integrated version to ease porting fixes between the two.
static status_t
SettingsFilePath(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append("Contacts");
	create_directory(path.Path(), 0755);
	path.Append("settings");
	return B_OK;
}


static void
LoadSavedFrame(BRect& frame)
{
	BPath path;
	if (SettingsFilePath(path) != B_OK)
		return;
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return;
	BMessage settings;
	if (settings.Unflatten(&file) != B_OK)
		return;
	BRect saved;
	if (settings.FindRect("contacts_frame", &saved) == B_OK
		&& saved.IsValid() && saved.Width() >= 480
		&& saved.Height() >= 240) {
		frame = saved;
	}
}


static bool
LoadColumnState(BMessage& outState)
{
	BPath path;
	if (SettingsFilePath(path) != B_OK)
		return false;
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() != B_OK)
		return false;
	BMessage settings;
	if (settings.Unflatten(&file) != B_OK)
		return false;
	return settings.FindMessage("contacts_columns", &outState) == B_OK;
}


static void
SaveWindowState(BRect frame, const BMessage& columnState)
{
	BPath path;
	if (SettingsFilePath(path) != B_OK)
		return;

	BMessage settings;
	BFile file(path.Path(), B_READ_ONLY);
	if (file.InitCheck() == B_OK)
		settings.Unflatten(&file);
	file.Unset();

	settings.RemoveName("contacts_frame");
	settings.AddRect("contacts_frame", frame);
	settings.RemoveName("contacts_columns");
	settings.AddMessage("contacts_columns", &columnState);

	file.SetTo(path.Path(),
		B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		settings.Flatten(&file);
}


// --- ContactsWindow ---


ContactsWindow::ContactsWindow()
	:
	BWindow(BRect(0, 0, 560, 420), B_TRANSLATE("Contacts"),
		B_TITLED_WINDOW,
		B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS),
	fImportPanel(NULL),
	fEscapeFilter(new KeyFilter(this))
{
	AddCommonFilter(fEscapeFilter);

	fFilterControl = new BTextControl("filter", "", "",
		new BMessage(kMsgFilterChanged));
	fFilterControl->SetModificationMessage(
		new BMessage(kMsgFilterChanged));
	fFilterControl->TextView()->SetExplicitMinSize(
		BSize(200, B_SIZE_UNSET));

	fListView = new ContactListView("contactList");
	fListView->AddColumn(new BStringColumn(B_TRANSLATE("Name"),
		180, 80, 400, B_TRUNCATE_END), 0);
	fListView->AddColumn(new BStringColumn(B_TRANSLATE("E-mail"),
		200, 80, 400, B_TRUNCATE_MIDDLE), 1);
	fListView->AddColumn(new BStringColumn(B_TRANSLATE("Company"),
		140, 60, 400, B_TRUNCATE_END), 2);
	fListView->AddColumn(new BStringColumn(B_TRANSLATE("Phone"),
		140, 60, 400, B_TRUNCATE_END), 3);
	fListView->AddColumn(new BStringColumn(B_TRANSLATE("URL"),
		160, 60, 400, B_TRUNCATE_MIDDLE), 4);
	// Phone and URL start hidden; right-click the list to toggle
	// columns. Saved state below overrides these defaults.
	fListView->ColumnAt(3)->SetVisible(false);
	fListView->ColumnAt(4)->SetVisible(false);
	fListView->SetSortColumn(fListView->ColumnAt(0), false, true);
	{
		BMessage columnState;
		if (LoadColumnState(columnState))
			fListView->LoadState(&columnState);
	}
	fListView->SetSelectionMessage(new BMessage(kMsgSelectionChanged));
	fListView->SetInvocationMessage(new BMessage(kMsgContactInvoked));
	fListView->SetExplicitMinSize(BSize(480, 240));
	// BColumnListView reports a constrained max width, which (via
	// B_AUTO_UPDATE_SIZE_LIMITS) pins the window's width; explicitly
	// unlimited so the window resizes horizontally too.
	fListView->SetExplicitMaxSize(
		BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));

	fAddButton = new BButton("new",
		B_TRANSLATE("New" B_UTF8_ELLIPSIS),
		new BMessage(kMsgAddContact));
	fEditButton = new BButton("open",
		B_TRANSLATE("Open" B_UTF8_ELLIPSIS),
		new BMessage(kMsgEditContact));
	fRemoveButton = new BButton("remove", B_TRANSLATE("Remove"),
		new BMessage(kMsgRemoveContact));
	fImportButton = new BButton("import",
		B_TRANSLATE("Import vCard" B_UTF8_ELLIPSIS),
		new BMessage(kMsgImportVCard));

	fCountLabel = new BStringView("count", "");
	fCountLabel->SetExplicitMaxSize(
		BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fCountLabel->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.AddGroup(B_HORIZONTAL)
			.Add(new BStringView("filterLabel", B_TRANSLATE("Filter:")))
			.Add(fFilterControl)
		.End()
		.Add(fListView)
		.AddGroup(B_HORIZONTAL)
			.Add(fAddButton)
			.Add(fEditButton)
			.Add(fRemoveButton)
			.AddGlue()
			.Add(fImportButton)
		.End()
		.Add(fCountLabel);

	BRect savedFrame(0, 0, -1, -1);
	LoadSavedFrame(savedFrame);
	if (savedFrame.IsValid()) {
		MoveTo(savedFrame.LeftTop());
		ResizeTo(savedFrame.Width(), savedFrame.Height());
		MoveOnScreen();
	} else
		CenterOnScreen();

	_StartQueries();
	_ApplyFilter();
	_UpdateButtons();
}


ContactsWindow::~ContactsWindow()
{
	RemoveCommonFilter(fEscapeFilter);
	delete fEscapeFilter;
	delete fImportPanel;
}


bool
ContactsWindow::QuitRequested()
{
	BMessage columnState;
	fListView->SaveState(&columnState);
	SaveWindowState(Frame(), columnState);
	_StopQueries();
	// Primary window: closing it quits the application.
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
ContactsWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgAddContact:
			_LaunchPeopleNew();
			break;

		case kMsgEditContact:
		case kMsgContactInvoked:
			_OpenSelectionInPeople();
			break;

		case kMsgRemoveContact:
			_RemoveSelection();
			break;

		case kMsgImportVCard:
			if (fImportPanel == NULL) {
				fImportPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), NULL, B_FILE_NODE, true);
				fImportPanel->Window()->SetTitle(
					B_TRANSLATE("Import vCard"));
			}
			fImportPanel->Show();
			break;

		case B_REFS_RECEIVED:
			_ImportRefs(message);
			break;

		case kMsgFilterChanged:
			_ApplyFilter();
			break;

		case kMsgSelectionChanged:
			_UpdateButtons();
			break;

		case B_QUERY_UPDATE:
		{
			int32 opcode = message->GetInt32("opcode", 0);
			if (opcode == B_ENTRY_CREATED) {
				entry_ref ref;
				const char* name = NULL;
				message->FindInt32("device", &ref.device);
				message->FindInt64("directory", &ref.directory);
				if (message->FindString("name", &name) == B_OK) {
					ref.set_name(name);
					_AddContact(ref);
				}
			} else if (opcode == B_ENTRY_REMOVED) {
				node_ref nodeRef;
				message->FindInt32("device", &nodeRef.device);
				message->FindInt64("node", &nodeRef.node);
				_RemoveContact(nodeRef);
			}
			break;
		}

		case B_NODE_MONITOR:
		{
			int32 opcode = message->GetInt32("opcode", 0);
			node_ref nodeRef;
			message->FindInt32("device", &nodeRef.device);
			message->FindInt64("node", &nodeRef.node);

			if (opcode == B_ATTR_CHANGED) {
				_UpdateContact(nodeRef);
			} else if (opcode == B_ENTRY_MOVED) {
				// A move can carry a contact into or out of the
				// Trash (still matching the type query either way),
				// or just rename it. Update the stored ref, then
				// re-evaluate.
				ContactRow* row = _RowFor(nodeRef);
				if (row == NULL)
					break;
				entry_ref newRef;
				const char* name = NULL;
				message->FindInt32("device", &newRef.device);
				message->FindInt64("to directory",
					&newRef.directory);
				if (message->FindString("name", &name) == B_OK)
					newRef.set_name(name);
				row->SetRef(newRef);
				if (IsInTrash(newRef))
					_RemoveContact(nodeRef);
				else
					_UpdateContact(nodeRef);
			}
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


// --- Query plumbing ---


void
ContactsWindow::_StartQueries()
{
	// One live query per persistent query-capable volume — the same
	// all-volumes policy as the mail views. Contacts created, edited,
	// or trashed by any application show up without a refresh.
	BVolumeRoster roster;
	BVolume volume;
	while (roster.GetNextVolume(&volume) == B_OK) {
		if (!volume.IsPersistent() || !volume.KnowsQuery())
			continue;

		BQuery* query = new BQuery();
		query->SetVolume(&volume);
		// BFS queries need at least one INDEXED attribute, and
		// BEOS:TYPE alone is not indexed — a type-only predicate
		// silently matches nothing. Anchor on the indexed META
		// attributes (same convention as the reader's autocomplete,
		// "META:email=**"); the type clause is then evaluated per
		// candidate. Note: on volumes without the META indexes,
		// contacts are invisible to this query — same limitation as
		// the autocomplete; the vCard importer creates the indexes on
		// its target volume for exactly this reason.
		query->SetPredicate("((META:name=\"**\")||(META:email=\"**\"))"
			"&&(BEOS:TYPE==\"application/x-person\")");
		query->SetTarget(BMessenger(this));
		if (query->Fetch() != B_OK) {
			delete query;
			continue;
		}
		fQueries.push_back(query);

		entry_ref ref;
		while (query->GetNextRef(&ref) == B_OK)
			_AddContact(ref);
	}
}


void
ContactsWindow::_StopQueries()
{
	for (size_t i = 0; i < fQueries.size(); i++) {
		fQueries[i]->Clear();
		delete fQueries[i];
	}
	fQueries.clear();

	stop_watching(this);
}


void
ContactsWindow::_AddContact(const entry_ref& ref)
{
	BEntry entry(&ref);
	if (!entry.Exists() || !entry.IsFile())
		return;
	if (IsInTrash(ref))
		return;

	node_ref nodeRef;
	if (entry.GetNodeRef(&nodeRef) != B_OK)
		return;
	if (_RowFor(nodeRef) != NULL)
		return;

	ContactRow* row = new ContactRow(nodeRef, ref);
	fContacts.push_back(row);

	// Watch attributes (People edits update the row live) and moves
	// (renames, trips into/out of the Trash).
	watch_node(&nodeRef, B_WATCH_ATTR | B_WATCH_NAME, this);

	if (row->MatchesFilter(fFilterControl->Text()))
		fListView->AddRow(row);
	_UpdateCountLabel();
}


void
ContactsWindow::_RemoveContact(const node_ref& nodeRef)
{
	for (size_t i = 0; i < fContacts.size(); i++) {
		ContactRow* row = fContacts[i];
		if (row->NodeRef() == nodeRef) {
			node_ref watched = nodeRef;
			watch_node(&watched, B_STOP_WATCHING, this);
			if (_RowAttached(row))
				fListView->RemoveRow(row);
			fContacts.erase(fContacts.begin() + i);
			delete row;
			_UpdateCountLabel();
			_UpdateButtons();
			return;
		}
	}
}


void
ContactsWindow::_UpdateContact(const node_ref& nodeRef)
{
	ContactRow* row = _RowFor(nodeRef);
	if (row == NULL)
		return;
	row->Reload();

	// In-place update: no list rebuild. Only filter membership
	// changes attach/detach the row; otherwise UpdateRow() redraws
	// and re-sorts just this one.
	bool attached = _RowAttached(row);
	bool matches = row->MatchesFilter(fFilterControl->Text());
	if (attached && !matches) {
		fListView->RemoveRow(row);
		_UpdateCountLabel();
		_UpdateButtons();
	} else if (!attached && matches) {
		fListView->AddRow(row);
		_UpdateCountLabel();
	} else if (attached) {
		fListView->UpdateRow(row);
	}
}


ContactRow*
ContactsWindow::_RowFor(const node_ref& nodeRef) const
{
	for (size_t i = 0; i < fContacts.size(); i++) {
		if (fContacts[i]->NodeRef() == nodeRef)
			return fContacts[i];
	}
	return NULL;
}


bool
ContactsWindow::_RowAttached(ContactRow* row) const
{
	for (int32 i = 0; i < fListView->CountRows(); i++) {
		if (fListView->RowAt(i) == row)
			return true;
	}
	return false;
}


void
ContactsWindow::_ApplyFilter()
{
	BString filter = fFilterControl->Text();

	// Detach everything (without deleting), then re-add matches.
	while (fListView->CountRows() > 0)
		fListView->RemoveRow(fListView->RowAt(0));

	for (size_t i = 0; i < fContacts.size(); i++) {
		if (fContacts[i]->MatchesFilter(filter))
			fListView->AddRow(fContacts[i]);
	}

	_UpdateCountLabel();
	_UpdateButtons();
}


void
ContactsWindow::_UpdateCountLabel()
{
	BString text;
	int32 shown = fListView->CountRows();
	int32 total = (int32)fContacts.size();
	if (shown == total) {
		text.SetToFormat(B_TRANSLATE("%d contact(s)"), (int)total);
	} else {
		text.SetToFormat(B_TRANSLATE("%d of %d contact(s)"),
			(int)shown, (int)total);
	}
	fCountLabel->SetText(text.String());
}


void
ContactsWindow::_UpdateButtons()
{
	int32 selected = 0;
	BRow* row = NULL;
	while ((row = fListView->CurrentSelection(row)) != NULL)
		selected++;

	fEditButton->SetEnabled(selected == 1);
	fRemoveButton->SetEnabled(selected > 0);
}


// --- Actions ---


void
ContactsWindow::_LaunchPeopleNew()
{
	// People launched plain opens its new-person window; nothing is
	// written to disk until the user saves there — cancel leaves no
	// residue. When People is already running, launching activates
	// it; the user starts a new person there (People's own
	// "New person" command).
	status_t status = be_roster->Launch(kPeopleAppSignature);
	if (status == B_ALREADY_RUNNING) {
		team_id team = be_roster->TeamFor(kPeopleAppSignature);
		if (team >= 0)
			be_roster->ActivateApp(team);
	}
}


void
ContactsWindow::_OpenSelectionInPeople()
{
	// Opens each selected contact in People (its preferred app);
	// works whether or not People is already running.
	BRow* row = NULL;
	while ((row = fListView->CurrentSelection(row)) != NULL) {
		ContactRow* contact = (ContactRow*)row;
		entry_ref ref = contact->Ref();
		be_roster->Launch(&ref);
	}
}


void
ContactsWindow::_RemoveSelection()
{
	// Collect first: removal mutates the selection.
	std::vector<entry_ref> refs;
	std::vector<node_ref> nodes;
	BRow* row = NULL;
	while ((row = fListView->CurrentSelection(row)) != NULL) {
		ContactRow* contact = (ContactRow*)row;
		refs.push_back(contact->Ref());
		nodes.push_back(contact->NodeRef());
	}
	if (refs.empty())
		return;

	BString text;
	if (refs.size() == 1) {
		text.SetToFormat(B_TRANSLATE("Move \"%s\" to the Trash?"),
			refs[0].name);
	} else {
		text.SetToFormat(
			B_TRANSLATE("Move %d contacts to the Trash?"),
			(int)refs.size());
	}
	BAlert* alert = new BAlert(B_TRANSLATE("Remove contacts"),
		text.String(), B_TRANSLATE("Cancel"), B_TRANSLATE("Remove"),
		NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	if (alert->Go() != 1)
		return;

	for (size_t i = 0; i < refs.size(); i++) {
		BEntry entry(&refs[i]);
		if (!entry.Exists())
			continue;

		BVolume volume(refs[i].device);
		BPath trashPath;
		if (find_directory(B_TRASH_DIRECTORY, &trashPath, true,
				&volume) != B_OK)
			continue;
		BDirectory trashDir(trashPath.Path());
		if (trashDir.InitCheck() != B_OK)
			continue;

		status_t status = entry.MoveTo(&trashDir, NULL, false);
		for (int n = 2; n <= 9 && status == B_FILE_EXISTS; n++) {
			BString alternate(refs[i].name);
			alternate << " " << n;
			status = entry.MoveTo(&trashDir, alternate.String(),
				false);
		}
		if (status == B_OK) {
			// The type query still matches files in the Trash, so no
			// removal event arrives — drop the row ourselves.
			_RemoveContact(nodes[i]);
		}
	}
}


void
ContactsWindow::_ImportRefs(BMessage* message)
{
	int32 imported = 0;
	int32 skipped = 0;
	BString errors;

	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		int32 fileImported = 0;
		int32 fileSkipped = 0;
		BString error;
		status_t status = VCardImporter::ImportFile(ref,
			&fileImported, &fileSkipped, &error);
		imported += fileImported;
		skipped += fileSkipped;
		if (status != B_OK && error.Length() > 0) {
			if (errors.Length() > 0)
				errors << "\n";
			errors << ref.name << ": " << error;
		}
	}

	BString text;
	text.SetToFormat(
		B_TRANSLATE("Imported %d contact(s), skipped %d duplicate(s)."),
		(int)imported, (int)skipped);
	if (skipped > 0) {
		BPath logPath;
		if (VCardImporter::LogPath(logPath) == B_OK) {
			text << "\n\n";
			BString note;
			note.SetToFormat(
				B_TRANSLATE("Skipped entries are listed in %s"),
				logPath.Path());
			text << note;
		}
	}
	if (errors.Length() > 0)
		text << "\n\n" << errors;

	BAlert* alert = new BAlert(B_TRANSLATE("Import vCard"),
		text.String(), B_TRANSLATE("OK"), NULL, NULL,
		B_WIDTH_AS_USUAL,
		errors.Length() > 0 ? B_WARNING_ALERT : B_INFO_ALERT);
	alert->Go(NULL);

	// New People files were created; the live queries deliver them to
	// the list on their own.
}
