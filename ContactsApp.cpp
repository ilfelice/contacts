/*
 * Contacts - browse, filter, and import Haiku People files
 *
 * Distributed under the terms of the MIT License.
 * Copyright 2026 Il Felice.
 */

#include "ContactsWindow.h"

#include <Application.h>
#include <Catalog.h>

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ContactsApp"


// Keep in sync with Contacts.rdef.
static const char* kAppSignature = "application/x-vnd.IlFelice-Contacts";


class ContactsApp : public BApplication {
public:
	ContactsApp()
		:
		BApplication(kAppSignature)
	{
	}

	virtual void ReadyToRun()
	{
		(new ContactsWindow())->Show();
	}
};


int
main()
{
	ContactsApp app;
	app.Run();
	return 0;
}
