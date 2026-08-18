// Extract.cpp

#include "StdAfx.h"

#include "../../../Common/StringConvert.h"

#include "../../../Windows/FileDir.h"
#include "../../../Windows/FileName.h"
#include "../../../Windows/ErrorMsg.h"
#include "../../../Windows/PropVariant.h"
#include "../../../Windows/PropVariantConv.h"

#include "../Common/ExtractingFilePath.h"
#include "../Common/HashCalc.h"

#include "Extract.h"
#include "SetProperties.h"

#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
#include "ZipRegistry.h"
#endif

using namespace NWindows;
using namespace NFile;
using namespace NDir;


static void SetErrorMessage(const char *message,
    const FString &path, HRESULT errorCode,
    UString &s)
{
  s = message;
  s += " : ";
  s += NError::MyFormatMessage(errorCode);
  s += " : ";
  s += fs2us(path);
}


#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)

// 同一批解压中，相同格式（FormatIndex）共享一个"组密码"缓存
struct CGroupPassword
{
  int FormatIndex;
  UString Password;   // 非空 = 该组命中的密码
  bool Found;         // true = 该组已完成密码测试（Password 可能为空 = 未命中）

  CGroupPassword(int formatIndex):
      FormatIndex(formatIndex),
      Found(false) {}
};

static CGroupPassword *FindGroupPassword(
    CObjectVector<CGroupPassword> &groups, int formatIndex)
{
  FOR_VECTOR (i, groups)
    if (groups[i].FormatIndex == formatIndex)
      return &groups[i];
  groups.Add(CGroupPassword(formatIndex));
  return &groups.Back();
}

static bool FindFirstEncryptedItem(IInArchive *archive, UInt32 &index)
{
  UInt32 numItems = 0;
  if (archive->GetNumberOfItems(&numItems) != S_OK)
    return false;
  for (UInt32 i = 0; i < numItems; i++)
  {
    bool isEncrypted = false;
    if (Archive_GetItemBoolProp(archive, i, kpidEncrypted, isEncrypted) == S_OK
        && isEncrypted)
    {
      index = i;
      return true;
    }
  }
  return false;
}

// 对单个条目做"测试解压"（testMode，不写盘），用于验证候选密码
static HRESULT TestOneItem_NoWrite(
    const CArchiveLink &arcLink,
    UInt32 itemIndex,
    const CExtractOptions &options,
    IFolderArchiveExtractCallback *faeCallback,
    CArchiveExtractCallback *ecs)
{
  const CArc &arc = arcLink.Arcs.Back();
  ecs->Init(options.NtOptions,
      NULL,             // wildcardCensor: 非 stdin 模式
      &arc,
      faeCallback,
      false,            // stdOutMode
      true,             // testMode: 只解码校验，不写盘
      FString(),        // directoryPath: 测试模式用不到
      UStringVector(),  // removePathParts
      false,            // removePartsForAltStreams
      0);               // packSize
  return arc.Archive->Extract(&itemIndex, 1, 1, ecs);
}

#endif


static HRESULT DecompressArchive(
    CCodecs *codecs,
    const CArchiveLink &arcLink,
    UInt64 packSize,
    const NWildcard::CCensorNode &wildcardCensor,
    const CExtractOptions &options,
    bool calcCrc,
    IExtractCallbackUI *callback,
    IFolderArchiveExtractCallback *callbackFAE,
    CArchiveExtractCallback *ecs,
    UString &errorMessage,
    UInt64 &stdInProcessed)
{
  const CArc &arc = arcLink.Arcs.Back();
  stdInProcessed = 0;
  IInArchive *archive = arc.Archive;
  CRecordVector<UInt32> realIndices;
  
  UStringVector removePathParts;

  FString outDir = options.OutputDir;
  if (options.OutDirMode != NExtractOutDirMode::k_Direct)
  {
    UString replaceName = arc.DefaultName;
    if (arcLink.Arcs.Size() > 1)
    {
      // Most "pe" archives have same name of archive subfile "[0]" or ".rsrc_1".
      // So it extracts different archives to one folder.
      // We will use top level archive name
      const CArc &arc0 = arcLink.Arcs[0];
      if (arc0.FormatIndex >= 0 && StringsAreEqualNoCase_Ascii(codecs->Formats[(unsigned)arc0.FormatIndex].Name, "pe"))
        replaceName = arc0.DefaultName;
    }
    const FString correctedName = us2fs(Get_Correct_FsFile_Name(replaceName));
    if (options.OutDirMode == NExtractOutDirMode::k_AddArcName)
    {
      outDir += correctedName;
      NFile::NName::NormalizeDirPathPrefix(outDir);
    }
    else // eo.OutDirMode == NExtractOutDirMode::k_ReplaceAsterisk;
      outDir.Replace(FString("*"), correctedName);
  }

  bool elimIsPossible = false;
  UString elimPrefix; // only pure name without dir delimiter
  FString outDirReduced = outDir;
  
  if (options.ElimDup.Val && options.PathMode != NExtract::NPathMode::kAbsPaths)
  {
    UString dirPrefix;
    SplitPathToParts_Smart(fs2us(outDir), dirPrefix, elimPrefix);
    if (!elimPrefix.IsEmpty())
    {
      if (IsPathSepar(elimPrefix.Back()))
        elimPrefix.DeleteBack();
      if (!elimPrefix.IsEmpty())
      {
        outDirReduced = us2fs(dirPrefix);
        elimIsPossible = true;
      }
    }
  }

  const bool allFilesAreAllowed = wildcardCensor.AreAllAllowed();

  if (!options.StdInMode)
  {
    UInt32 numItems;
    RINOK(archive->GetNumberOfItems(&numItems))
    
    CReadArcItem item;

    for (UInt32 i = 0; i < numItems; i++)
    {
      if (elimIsPossible
          || !allFilesAreAllowed
          || options.ExcludeDirItems
          || options.ExcludeFileItems)
      {
        RINOK(arc.GetItem(i, item))
        if (item.IsDir ? options.ExcludeDirItems : options.ExcludeFileItems)
          continue;
      }
      else
      {
        #ifdef SUPPORT_ALT_STREAMS
        item.IsAltStream = false;
        if (!options.NtOptions.AltStreams.Val && arc.Ask_AltStream)
        {
          RINOK(Archive_IsItem_AltStream(arc.Archive, i, item.IsAltStream))
        }
        #endif
      }

      #ifdef SUPPORT_ALT_STREAMS
      if (!options.NtOptions.AltStreams.Val && item.IsAltStream)
        continue;
      #endif
      
      if (elimIsPossible)
      {
        const UString &s =
          #ifdef SUPPORT_ALT_STREAMS
            item.MainPath;
          #else
            item.Path;
          #endif
        if (!IsPath1PrefixedByPath2(s, elimPrefix))
          elimIsPossible = false;
        else
        {
          wchar_t c = s[elimPrefix.Len()];
          if (c == 0)
          {
            if (!item.MainIsDir)
              elimIsPossible = false;
          }
          else if (!IsPathSepar(c))
            elimIsPossible = false;
        }
      }

      if (!allFilesAreAllowed)
      {
        if (!CensorNode_CheckPath(wildcardCensor, item))
          continue;
      }

      realIndices.Add(i);
    }
    
    if (realIndices.Size() == 0)
    {
      callback->ThereAreNoFiles();
      return callback->ExtractResult(S_OK);
    }
  }

  if (elimIsPossible)
  {
    removePathParts.Add(elimPrefix);
    // outDir = outDirReduced;
  }

  #ifdef _WIN32
  // GetCorrectFullFsPath doesn't like "..".
  // outDir.TrimRight();
  // outDir = GetCorrectFullFsPath(outDir);
  #endif

  if (outDir.IsEmpty())
    outDir = "." STRING_PATH_SEPARATOR;
  /*
  #ifdef _WIN32
  else if (NName::IsAltPathPrefix(outDir)) {}
  #endif
  */
  else if (!CreateComplexDir(outDir))
  {
    const HRESULT res = GetLastError_noZero_HRESULT();
    SetErrorMessage("Cannot create output directory", outDir, res, errorMessage);
    return res;
  }

  ecs->Init(
      options.NtOptions,
      options.StdInMode ? &wildcardCensor : NULL,
      &arc,
      callbackFAE,
      options.StdOutMode, options.TestMode,
      outDir,
      removePathParts, false,
      packSize);

  ecs->Is_elimPrefix_Mode = elimIsPossible;

  
  #ifdef SUPPORT_LINKS
  
  if (!options.StdInMode &&
      !options.TestMode &&
      options.NtOptions.HardLinks.Val)
  {
    RINOK(ecs->PrepareHardLinks(&realIndices))
  }
    
  #endif

  
  HRESULT result;
  const Int32 testMode = (options.TestMode && !calcCrc) ? 1: 0;

  CArchiveExtractCallback_Closer ecsCloser(ecs);

  if (options.StdInMode)
  {
    result = archive->Extract(NULL, (UInt32)(Int32)-1, testMode, ecs);
    NCOM::CPropVariant prop;
    if (archive->GetArchiveProperty(kpidPhySize, &prop) == S_OK)
      ConvertPropVariantToUInt64(prop, stdInProcessed);
  }
  else
  {
    // v23.02: we reset completed value that could be set by Open() operation
    IArchiveExtractCallback *aec = ecs;
    const UInt64 val = 0;
    RINOK(aec->SetCompleted(&val))
    result = archive->Extract(realIndices.ConstData(), realIndices.Size(), testMode, aec);
  }
  
  const HRESULT res2 = ecsCloser.Close();
  if (result == S_OK)
    result = res2;

  return callback->ExtractResult(result);
}

/* v9.31: BUG was fixed:
   Sorted list for file paths was sorted with case insensitive compare function.
   But FindInSorted function did binary search via case sensitive compare function */

int Find_FileName_InSortedVector(const UStringVector &fileNames, const UString &name);
int Find_FileName_InSortedVector(const UStringVector &fileNames, const UString &name)
{
  unsigned left = 0, right = fileNames.Size();
  while (left != right)
  {
    const unsigned mid = (unsigned)(((size_t)left + (size_t)right) / 2);
    const UString &midVal = fileNames[mid];
    const int comp = CompareFileNames(name, midVal);
    if (comp == 0)
      return (int)mid;
    if (comp < 0)
      right = mid;
    else
      left = mid + 1;
  }
  return -1;
}



HRESULT Extract(
    // DECL_EXTERNAL_CODECS_LOC_VARS
    CCodecs *codecs,
    const CObjectVector<COpenType> &types,
    const CIntVector &excludedFormats,
    UStringVector &arcPaths, UStringVector &arcPathsFull,
    const NWildcard::CCensorNode &wildcardCensor,
    const CExtractOptions &options,
    IOpenCallbackUI *openCallback,
    IExtractCallbackUI *extractCallback,
    IFolderArchiveExtractCallback *faeCallback,
    #ifndef Z7_SFX
    IHashCalc *hash,
    #endif
    UString &errorMessage,
    CDecompressStat &st)
{
  st.Clear();
  UInt64 totalPackSize = 0;
  CRecordVector<UInt64> arcSizes;

#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
  // 密码箱数据只读取一次（DPAPI 解密成本高）
  UStringVector vaultPasswords;
  if (options.TryPasswordVault && !options.StdInMode)
    NVault::Read_PasswordVault(vaultPasswords);
  CObjectVector<CGroupPassword> groupPasswords;
#endif

  unsigned numArcs = options.StdInMode ? 1 : arcPaths.Size();

  unsigned i;
  
  for (i = 0; i < numArcs; i++)
  {
    NFind::CFileInfo fi;
    fi.Size = 0;
    if (!options.StdInMode)
    {
      const FString arcPath = us2fs(arcPaths[i]);
      if (!fi.Find_FollowLink(arcPath))
      {
        const HRESULT errorCode = GetLastError_noZero_HRESULT();
        SetErrorMessage("Cannot find archive file", arcPath, errorCode, errorMessage);
        return errorCode;
      }
      if (fi.IsDir())
      {
        HRESULT errorCode = E_FAIL;
        SetErrorMessage("The item is a directory", arcPath, errorCode, errorMessage);
        return errorCode;
      }
    }
    arcSizes.Add(fi.Size);
    totalPackSize += fi.Size;
  }

  CBoolArr skipArcs(numArcs);
  for (i = 0; i < numArcs; i++)
    skipArcs[i] = false;

  CArchiveExtractCallback *ecs = new CArchiveExtractCallback;
  CMyComPtr<IArchiveExtractCallback> ec(ecs);
  
  const bool multi = (numArcs > 1);
  
  ecs->InitForMulti(multi,
      options.PathMode,
      options.OverwriteMode,
      options.ZoneMode,
      false // keepEmptyDirParts
      );
  #ifndef Z7_SFX
  ecs->SetHashMethods(hash);
  #endif

  if (multi)
  {
    RINOK(faeCallback->SetTotal(totalPackSize))
  }

  UInt64 totalPackProcessed = 0;
  bool thereAreNotOpenArcs = false;
  
  for (i = 0; i < numArcs; i++)
  {
    if (skipArcs[i])
      continue;

    ecs->InitBeforeNewArchive();

    const UString &arcPath = arcPaths[i];
    NFind::CFileInfo fi;
    if (options.StdInMode)
    {
      // do we need ctime and mtime?
      // fi.ClearBase();
      // fi.Size = 0; // (UInt64)(Int64)-1;
      if (!fi.SetAs_StdInFile())
        return GetLastError_noZero_HRESULT();
    }
    else
    {
      if (!fi.Find_FollowLink(us2fs(arcPath)) || fi.IsDir())
      {
        const HRESULT errorCode = GetLastError_noZero_HRESULT();
        SetErrorMessage("Cannot find archive file", us2fs(arcPath), errorCode, errorMessage);
        return errorCode;
      }
    }

    /*
    #ifndef Z7_NO_CRYPTO
    openCallback->Open_Clear_PasswordWasAsked_Flag();
    #endif
    */

    RINOK(extractCallback->BeforeOpen(arcPath, options.TestMode))
    CArchiveLink arcLink;

    CObjectVector<COpenType> types2 = types;
    /*
    #ifndef Z7_SFX
    if (types.IsEmpty())
    {
      int pos = arcPath.ReverseFind(L'.');
      if (pos >= 0)
      {
        UString s = arcPath.Ptr(pos + 1);
        int index = codecs->FindFormatForExtension(s);
        if (index >= 0 && s.IsEqualTo("001"))
        {
          s = arcPath.Left(pos);
          pos = s.ReverseFind(L'.');
          if (pos >= 0)
          {
            int index2 = codecs->FindFormatForExtension(s.Ptr(pos + 1));
            if (index2 >= 0) // && s.CompareNoCase(L"rar") != 0
            {
              types2.Add(index2);
              types2.Add(index);
            }
          }
        }
      }
    }
    #endif
    */

    COpenOptions op;
    #ifndef Z7_SFX
    op.props = &options.Properties;
    #endif
    op.codecs = codecs;
    op.types = &types2;
    op.excludedFormats = &excludedFormats;
    op.stdInMode = options.StdInMode;
    op.stream = NULL;
    op.filePath = arcPath;

    HRESULT result = E_FAIL;

#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
    const bool vaultEnabled = options.TryPasswordVault && !options.StdInMode
        && vaultPasswords.Size() != 0;
    UString vaultFoundPassword;       // 通过候选"试打开"命中的密码
    bool vaultOpenedViaCandidate = false;
    bool vaultInjectedPsw = false;    // 当前包是否注入了密码箱密码（包结束后清除）
    bool groupPswWasInjected = false; // 当前包是否注入了"组密码"
    bool representativeTestedThisArc = false; // 当前包是否已做过代表测试

    if (vaultEnabled && !extractCallback->Get_PasswordIsDefined())
    {
      // 密码箱优先：静默尝试候选密码，避免逐个弹密码框
      extractCallback->SetTryMode(true);
      FOR_VECTOR (k, vaultPasswords)
      {
        extractCallback->SetPassword(vaultPasswords[k]);
        extractCallback->SetPasswordDefined(true);
        result = arcLink.Open_Strict(op, openCallback);
        if (result == E_ABORT)
          break; // 用户取消
        if (result == S_OK)
        {
          vaultFoundPassword = vaultPasswords[k];
          vaultOpenedViaCandidate = true;
          break;
        }
        if (!arcLink.PasswordWasAsked)
          break; // 打开失败与密码无关（损坏/格式不符）：不再浪费候选
      }
      extractCallback->SetTryMode(false);
      if (!vaultOpenedViaCandidate)
      {
        // 全部候选失败：清空注入，走原版流程（必要时弹原版密码框）
        extractCallback->SetPasswordDefined(false);
        if (result != E_ABORT)
          result = arcLink.Open_Strict(op, openCallback);
      }
    }
    else
#endif
      result = arcLink.Open_Strict(op, openCallback);

    if (result == E_ABORT)
      return result;

    // arcLink.Set_ErrorsText();
    RINOK(extractCallback->OpenResult(codecs, arcLink, arcPath, result))

    if (result != S_OK)
    {
      thereAreNotOpenArcs = true;
      if (!options.StdInMode)
        totalPackProcessed += fi.Size;
      continue;
    }

   #if defined(_WIN32) && !defined(UNDER_CE) && !defined(Z7_SFX)
    if (options.ZoneMode != NExtract::NZoneIdMode::kNone
        && !options.StdInMode)
    {
      ReadZoneFile_Of_BaseFile(us2fs(arcPath), ecs->ZoneBuf);
    }
   #endif
    

    if (arcLink.Arcs.Size() != 0)
    {
      if (arcLink.GetArc()->IsHashHandler(op))
      {
        if (!options.TestMode)
        {
          /* real Extracting to files is possible.
             But user can think that hash archive contains real files.
             So we block extracting here. */
          // v23.00 : we don't break process.
          RINOK(extractCallback->OpenResult(codecs, arcLink, arcPath, E_NOTIMPL))
          thereAreNotOpenArcs = true;
          if (!options.StdInMode)
            totalPackProcessed += fi.Size;
          continue;
          // return E_NOTIMPL; // before v23
        }
        FString dirPrefix = us2fs(options.HashDir);
        if (dirPrefix.IsEmpty())
        {
          if (!NFile::NDir::GetOnlyDirPrefix(us2fs(arcPath), dirPrefix))
          {
            // return GetLastError_noZero_HRESULT();
          }
        }
        if (!dirPrefix.IsEmpty())
          NName::NormalizeDirPathPrefix(dirPrefix);
        ecs->DirPathPrefix_for_HashFiles = dirPrefix;
      }
    }

    if (!options.StdInMode)
    {
      // numVolumes += arcLink.VolumePaths.Size();
      // arcLink.VolumesSize;

      // totalPackSize -= DeleteUsedFileNamesFromList(arcLink, i + 1, arcPaths, arcPathsFull, &arcSizes);
      // numArcs = arcPaths.Size();
      if (arcLink.VolumePaths.Size() != 0)
      {
        Int64 correctionSize = (Int64)arcLink.VolumesSize;
        FOR_VECTOR (v, arcLink.VolumePaths)
        {
          int index = Find_FileName_InSortedVector(arcPathsFull, arcLink.VolumePaths[v]);
          if (index >= 0)
          {
            if ((unsigned)index > i)
            {
              skipArcs[(unsigned)index] = true;
              correctionSize -= arcSizes[(unsigned)index];
            }
          }
        }
        if (correctionSize != 0)
        {
          Int64 newPackSize = (Int64)totalPackSize + correctionSize;
          if (newPackSize < 0)
            newPackSize = 0;
          totalPackSize = (UInt64)newPackSize;
          RINOK(faeCallback->SetTotal(totalPackSize))
        }
      }
    }

    /*
    // Now openCallback and extractCallback use same object. So we don't need to send password.

    #ifndef Z7_NO_CRYPTO
    bool passwordIsDefined;
    UString password;
    RINOK(openCallback->Open_GetPasswordIfAny(passwordIsDefined, password))
    if (passwordIsDefined)
    {
      RINOK(extractCallback->SetPassword(password))
    }
    #endif
    */

    // ---- 密码箱分组自动测试（F2）----
#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
    if (vaultEnabled && result == S_OK && arcLink.Arcs.Size() != 0)
    {
      const int fmt = arcLink.Arcs.Back().FormatIndex;
      if (fmt >= 0)
      {
        CGroupPassword *grp = FindGroupPassword(groupPasswords, fmt);

        if (!grp->Found)
        {
          // 该包成为（或顺延为）组代表：找第一个加密条目做单文件测试
          UInt32 encIndex;
          if (FindFirstEncryptedItem(arcLink.Arcs.Back().Archive, encIndex))
          {
            representativeTestedThisArc = true;
            FOR_VECTOR (k, vaultPasswords)
            {
              extractCallback->ResetErrors();
              extractCallback->SetTryMode(true);
              extractCallback->SetPassword(vaultPasswords[k]);
              extractCallback->SetPasswordDefined(true);
              const HRESULT testRes = TestOneItem_NoWrite(arcLink, encIndex, options,
                  faeCallback, ecs);
              if (testRes == S_OK && extractCallback->Get_TryIsOK())
              {
                grp->Found = true;
                grp->Password = vaultPasswords[k];
                break;
              }
              if (testRes == E_ABORT)
                break;
            }
            extractCallback->SetTryMode(false);
            extractCallback->ResetErrors(); // 试测错误不计入正式解压
            if (!grp->Found)
            {
              // 候选全部失败：该组不再重复测试，正式解压时走原版密码框
              grp->Found = true;
              grp->Password.Empty();
              extractCallback->SetPasswordDefined(false);
            }
          }
          else if (vaultOpenedViaCandidate)
          {
            // 非加密包：不保留候选密码，也不占用组代表名额
            extractCallback->SetPasswordDefined(false);
          }
        }

        if (grp->Found && !grp->Password.IsEmpty())
        {
          // 注入组密码（组内其它包直接复用，不再测试）
          extractCallback->SetPassword(grp->Password);
          extractCallback->SetPasswordDefined(true);
          groupPswWasInjected = true;
          vaultInjectedPsw = true;
        }
      }
    }
#endif

    CArc &arc = arcLink.Arcs.Back();
    arc.MTime.Def = !options.StdInMode
        #ifdef _WIN32
        && !fi.IsDevice
        #endif
        ;
    if (arc.MTime.Def)
      arc.MTime.Set_From_FiTime(fi.MTime);

    UInt64 packProcessed;
    const bool calcCrc =
        #ifndef Z7_SFX
          (hash != NULL);
        #else
          false;
        #endif

    // F3/F4 按"当前包"判定：比较正式解压前后的批次错误计数
    UInt32 errBeforeExtract = extractCallback->Get_NumBatchErrors();

    RINOK(DecompressArchive(
        codecs,
        arcLink,
        fi.Size + arcLink.VolumesSize,
        wildcardCensor,
        options,
        calcCrc,
        extractCallback, faeCallback, ecs,
        errorMessage, packProcessed))

#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
    // F3：组密码对该包失效时，单独重新测试密码箱
    if (vaultEnabled && groupPswWasInjected && !representativeTestedThisArc
        && result != E_ABORT
        && extractCallback->Get_NumBatchErrors() > errBeforeExtract
        && arcLink.Arcs.Size() != 0)
    {
      UInt32 encIndex;
      if (FindFirstEncryptedItem(arcLink.Arcs.Back().Archive, encIndex))
      {
        // 保存计数：重测+重跑后不污染批次统计
        const UInt64 saveNumFolders = ecs->NumFolders;
        const UInt64 saveNumFiles = ecs->NumFiles;
        const UInt64 saveNumAltStreams = ecs->NumAltStreams;
        const UInt64 saveUnpackSize = ecs->UnpackSize;
        const UInt64 saveAltUnpackSize = ecs->AltStreams_UnpackSize;
        const UInt64 saveInSize = ecs->LocalProgressSpec->InSize;

        UString newPsw;
        FOR_VECTOR (k, vaultPasswords)
        {
          extractCallback->ResetErrors();
          extractCallback->SetTryMode(true);
          extractCallback->SetPassword(vaultPasswords[k]);
          extractCallback->SetPasswordDefined(true);
          const HRESULT testRes = TestOneItem_NoWrite(arcLink, encIndex, options,
              faeCallback, ecs);
          if (testRes == S_OK && extractCallback->Get_TryIsOK())
          {
            newPsw = vaultPasswords[k];
            break;
          }
          if (testRes == E_ABORT)
            break;
        }
        extractCallback->SetTryMode(false);
        extractCallback->ResetErrors();

        // 恢复第一次失败解压前的计数
        ecs->NumFolders = saveNumFolders;
        ecs->NumFiles = saveNumFiles;
        ecs->NumAltStreams = saveNumAltStreams;
        ecs->UnpackSize = saveUnpackSize;
        ecs->AltStreams_UnpackSize = saveAltUnpackSize;
        ecs->LocalProgressSpec->InSize = saveInSize;

        if (!newPsw.IsEmpty())
        {
          // 命中新密码：重跑一次正式解压
          extractCallback->SetPassword(newPsw);
          extractCallback->SetPasswordDefined(true);
          vaultInjectedPsw = true;
          errBeforeExtract = extractCallback->Get_NumBatchErrors();
          RINOK(DecompressArchive(
              codecs, arcLink, fi.Size + arcLink.VolumesSize,
              wildcardCensor, options, calcCrc,
              extractCallback, faeCallback, ecs,
              errorMessage, packProcessed))
        }
        else
        {
          // 未命中：清空注入，重跑正式解压（走原版弹框）
          extractCallback->SetPasswordDefined(false);
          errBeforeExtract = extractCallback->Get_NumBatchErrors();
          RINOK(DecompressArchive(
              codecs, arcLink, fi.Size + arcLink.VolumesSize,
              wildcardCensor, options, calcCrc,
              extractCallback, faeCallback, ecs,
              errorMessage, packProcessed))
        }
      }
    }
#endif

    const UInt32 errAfterExtract = extractCallback->Get_NumBatchErrors();

    if (!options.StdInMode)
      packProcessed = fi.Size + arcLink.VolumesSize;
    totalPackProcessed += packProcessed;
    ecs->LocalProgressSpec->InSize += packProcessed;
    ecs->LocalProgressSpec->OutSize = ecs->UnpackSize;
    if (!errorMessage.IsEmpty())
      return E_FAIL;

    // F4：解压成功后删除压缩包（含分卷）
    if (options.DeleteAfterExtract
        && !options.TestMode
        && !options.StdInMode
        && result == S_OK
        && errAfterExtract == errBeforeExtract)
    {
      // arcLink 仍持有压缩包的文件句柄，直接删会被 Windows 拒绝。
      // 先拷贝分卷路径并释放 arcLink，再删除主文件+全部分卷。
      UStringVector volumePaths = arcLink.VolumePaths;
      arcLink.Release();
      NDir::DeleteFileAlways(us2fs(arcPath));
      FOR_VECTOR (v, volumePaths)
        NDir::DeleteFileAlways(us2fs(volumePaths[v]));
    }

#if !defined(Z7_NO_CRYPTO) && !defined(Z7_NO_REGISTRY)
    // 密码箱注入的密码只对当前包有效；用户手动输入的密码保持原版跨包行为
    if (vaultInjectedPsw)
      extractCallback->SetPasswordDefined(false);
#endif
  }

  if (multi || thereAreNotOpenArcs)
  {
    RINOK(faeCallback->SetTotal(totalPackSize))
    RINOK(faeCallback->SetCompleted(&totalPackProcessed))
  }

  st.NumFolders = ecs->NumFolders;
  st.NumFiles = ecs->NumFiles;
  st.NumAltStreams = ecs->NumAltStreams;
  st.UnpackSize = ecs->UnpackSize;
  st.AltStreams_UnpackSize = ecs->AltStreams_UnpackSize;
  st.NumArchives = arcPaths.Size();
  st.PackSize = ecs->LocalProgressSpec->InSize;
  return S_OK;
}
