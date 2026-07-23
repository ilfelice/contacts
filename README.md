# Contacts

This is a small utility native Haiku program designed to manage your Person file contacts in a single place (search, edit, delete, add, import). A vCard-to-Person files import feature is included. Forked from the Contacts feature of EmailViewsNeo.

## Features

- Live list of every People file on all query-capable volumes.
- Filter by name, e-mail, or nickname
- Sortable columns (Name, E-mail, Company, Phone, URL); right-click the list header to toggle column visibility; drag column headers to reorder. Window size, position, and column arrangement persist.
- Uses People app for editing and new contact creation ("New" button opens People's blank person window, "Open" button/double-click opens selection).
- "Remove" button and Delete key moves selected contact(s) to the system Trash, with confirmation (restoring from the Trash brings them back to the list).
- vCard import (2.1 / 3.0 / 4.0; Gmail, iCloud, and Thunderbird exports): full People attribute mapping, duplicate detection by e-mail (case-insensitive) with a name fallback for contacts without an e-mail address, and a per-run log with skip reasons at '~/config/settings/Contacts/contacts_import.log'.

## Requirements

Developed in 64-bit Haiku, nightly version. Not tested on Beta 5 or 32-bit Haiku.
