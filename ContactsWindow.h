/*
 * Contacts - ContactsWindow
 *
 * Distributed under the terms of the MIT License.
 * Copyright 2026 Il Felice.
 *
 * Light contact management over Haiku People files
 * (application/x-person). The window is a live-query browser plus a
 * vCard importer; all editing happens in the system People app:
 *   - the list is fed by live queries over every query-capable
 *     volume, so contacts created, edited, or removed by ANY app
 *     appear/update/vanish automatically;
 *   - "Add" launches People plain (its new-person window; nothing is
 *     written until the user saves there);
 *   - "Edit" / double-click open the selection in People;
 *   - "Remove" moves the files to the Trash;
 *   - "Import vCard" feeds .vcf files to VCardImporter.
 */

#ifndef CONTACTS_WINDOW_H
#define CONTACTS_WINDOW_H

#include <Entry.h>
#include <Node.h>
#include <String.h>
#include <Window.h>

#include <vector>

class BButton;
class BColumnListView;
class BFilePanel;
class BMessageFilter;
class BQuery;
class BRow;
class BStringView;
class BTextControl;
class ContactRow;

class ContactsWindow : public BWindow {
public:
								ContactsWindow();
	virtual						~ContactsWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();

private:
			void				_StartQueries();
			void				_StopQueries();
			void				_AddContact(const entry_ref& ref);
			void				_RemoveContact(const node_ref& nodeRef);
			void				_UpdateContact(const node_ref& nodeRef);
			ContactRow*			_RowFor(const node_ref& nodeRef) const;
			bool				_RowAttached(ContactRow* row) const;
			void				_ApplyFilter();
			void				_UpdateCountLabel();
			void				_UpdateButtons();

			void				_LaunchPeopleNew();
			void				_OpenSelectionInPeople();
			void				_RemoveSelection();
			void				_ImportRefs(BMessage* message);

private:
			BTextControl*		fFilterControl;
			BColumnListView*	fListView;
			BButton*			fAddButton;
			BButton*			fEditButton;
			BButton*			fRemoveButton;
			BButton*			fImportButton;
			BStringView*		fCountLabel;
			BFilePanel*			fImportPanel;
			BMessageFilter*		fEscapeFilter;

			std::vector<BQuery*> fQueries;

			// All known contacts (unfiltered); rows are created and
			// destroyed by the filter. The window owns the rows not
			// currently attached to the list view.
			std::vector<ContactRow*> fContacts;
};


#endif // CONTACTS_WINDOW_H
