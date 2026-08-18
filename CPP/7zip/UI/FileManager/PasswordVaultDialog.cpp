#include "StdAfx.h"

#include "PasswordVaultDialog.h"
#include "../Common/ZipRegistry.h"
#include "PasswordDialog.h"

using namespace NWindows;

void CPasswordVaultDialog::Save()
{
  NVault::Save_PasswordVault(_passwords);
}

void CPasswordVaultDialog::Reload()
{
  _list.DeleteAllItems();
  FOR_VECTOR (i, _passwords)
	_list.InsertItem((unsigned)i, _show ? _passwords[i].Ptr() : L"********");
}

bool CPasswordVaultDialog::OnInit()
{
  _list.Attach(GetItem(IDL_PASSWORD_VAULT));
  _list.SetUnicodeFormat();
  _list.InsertColumn(0, L"Passwords", 200);
  NVault::Read_PasswordVault(_passwords);
  Reload();
	CheckButton(IDX_PASSWORD_VAULT_SHOW, false);
  return CModalDialog::OnInit();
}

bool CPasswordVaultDialog::OnButtonClicked(unsigned buttonID, HWND buttonHWND)
{
  switch (buttonID)
  {
	case IDX_PASSWORD_VAULT_SHOW:
	  _show = IsButtonCheckedBool(IDX_PASSWORD_VAULT_SHOW);
	  Reload();
	  return true;

	case IDB_PASSWORD_VAULT_ADD:
	{
		CPasswordDialog dlg;                       // reuse existing password input dialog
	  if (dlg.Create(*this) == IDOK && !dlg.Password.IsEmpty())
	  {
		_passwords.Insert(0, dlg.Password);      // newest first
		if (_passwords.Size() > 10)
		  _passwords.DeleteBack();
		Save();
		Reload();
	  }
	  return true;
	}

	case IDB_PASSWORD_VAULT_DELETE:
	{
	  const int index = _list.GetNextSelectedItem(-1);
	  if (index >= 0
		  && ::MessageBoxW(*this, L"Delete this password?", L"Password Vault", MB_YESNO) == IDYES)
	  {
		_passwords.Delete((unsigned)index);
		Save();
		Reload();
	  }
	  return true;
	}

	case IDB_PASSWORD_VAULT_CLEAR:
	  if (::MessageBoxW(*this, L"Clear all passwords?", L"Password Vault", MB_YESNO) == IDYES)
	  {
		_passwords.Clear();
		Save();
		Reload();
	  }
	  return true;
  }
  return CModalDialog::OnButtonClicked(buttonID, buttonHWND);
}
