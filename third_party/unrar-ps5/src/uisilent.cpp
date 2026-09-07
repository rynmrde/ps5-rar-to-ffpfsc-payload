// Purely user interface function. Gets and returns user input.
extern "C" int MkpfsArchiveCancelled(void);
extern "C" void Ps5UnrarProgress(int Percent);
UIASKREP_RESULT uiAskReplace(std::wstring &Name,int64 FileSize,RarTime *FileTime,uint Flags)
{
  return UIASKREP_R_REPLACE;
}




void uiStartArchiveExtract(bool Extract,const std::wstring &ArcName)
{
}


bool uiStartFileExtract(const std::wstring &FileName,bool Extract,bool Test,bool Skip)
{
  return !MkpfsArchiveCancelled();
}


void uiExtractProgress(int64 CurFileSize,int64 TotalFileSize,int64 CurSize,int64 TotalSize)
{
  if (TotalSize>0)
    Ps5UnrarProgress((int)((CurSize*100)/TotalSize));
  else if (TotalFileSize>0)
    Ps5UnrarProgress((int)((CurFileSize*100)/TotalFileSize));
}


void uiProcessProgress(const char *Command,int64 CurSize,int64 TotalSize)
{
}


void uiMsgStore::Msg()
{
}


bool uiGetPassword(UIPASSWORD_TYPE Type,const std::wstring &FileName,
                   SecPassword *Password,CheckPassword *CheckPwd)
{
  return false;
}


bool uiIsGlobalPasswordSet()
{
  return false;
}


void uiAlarm(UIALARM_TYPE Type)
{
}


bool uiIsAborted()
{
  return MkpfsArchiveCancelled()!=0;
}


void uiGiveTick()
{
}


bool uiDictLimit(CommandData *Cmd,const std::wstring &FileName,uint64 DictSize,uint64 MaxDictSize)
{
#ifdef RARDLL
  if (Cmd->Callback!=nullptr &&
      Cmd->Callback(UCM_LARGEDICT,Cmd->UserData,(LPARAM)(DictSize/1024),(LPARAM)(MaxDictSize/1024))==1)
    return true; // Continue extracting if unrar.dll callback permits it.
#endif
  return false; // Stop extracting.
}


#ifndef SFX_MODULE
const wchar *uiGetMonthName(uint Month)
{
  return L"";
}


const wchar *uiGetWeekDayName(uint Day)
{
  return L"";
}
#endif


void uiEolAfterMsg()
{
}
