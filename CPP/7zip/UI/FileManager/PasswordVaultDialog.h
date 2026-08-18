#ifndef ZIP7_INC_PASSWORD_VAULT_DIALOG_H
#define ZIP7_INC_PASSWORD_VAULT_DIALOG_H

#include "../../../Windows/Control/Dialog.h"
#include "../../../Windows/Control/ListView.h"

#include "PasswordVaultDialogRes.h"

class CPasswordVaultDialog: public NWindows::NControl::CModalDialog
{
  NWindows::NControl::CListView _list;
  UStringVector _passwords;
  bool _show;
  void Save();
  void Reload();
  virtual bool OnInit() Z7_override;
  virtual bool OnButtonClicked(unsigned buttonID, HWND buttonHWND) Z7_override;
public:
  CPasswordVaultDialog(): _show(false) {}
  INT_PTR Create(HWND wndParent = NULL) { return CModalDialog::Create(IDD_PASSWORD_VAULT, wndParent); }
};

#endif
