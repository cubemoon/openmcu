
#include <ptlib.h>
#include <stdio.h>
#include <string.h>

#include "conference.h"
#include "mcu.h"

extern "C" {
#if USE_SWRESAMPLE
#include <libswresample/swresample.h>
#include <libavutil/audioconvert.h>
#elif USE_AVRESAMPLE
#include <libavresample/avresample.h>
#include <libavutil/audioconvert.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libavutil/mem.h>
#elif USE_LIBSAMPLERATE
#include <samplerate.h>
#endif
};

#if OPENMCU_VIDEO
#include <ptlib/vconvert.h>
#endif

// size of a PCM data packet, in samples
//#define PCM_PACKET_LEN          480
//#define PCM_PACKET_LEN          1920
#define PCM_BUFFER_LEN_MS /*ms */ 120

// size of a PCM data buffer, in bytes
//#define PCM_BUFFER_LEN          (PCM_PACKET_LEN * 2)

// number of PCM buffers to keep
#define PCM_BUFFER_COUNT        2

#define PCM_BUFFER_SIZE_CALC(freq,chans)\
  bufferSize = 2/* bytes*/ * chans * PCM_BUFFER_LEN_MS * PCM_BUFFER_COUNT * freq / 1000;\
  if(bufferSize < 4) bufferSize=200;\
  buffer.SetSize(bufferSize + 16);


//#define PCM_BUFFER_SIZE         (PCM_BUFFER_LEN * PCM_BUFFER_COUNT)

////////////////////////////////////////////////////////////////////////////////////

ConferenceManager::ConferenceManager()
{
  maxConferenceCount = 0;
  monitor  = new ConferenceMonitor(*this);
}

ConferenceManager::~ConferenceManager()
{
  monitor->running = FALSE;
  monitor->WaitForTermination();
  delete monitor;
}


Conference * ConferenceManager::MakeAndLockConference(const PString & roomToCreate, const PString & name)
{
  PWaitAndSignal m(conferenceListMutex);
  OpalGloballyUniqueID conferenceID;
  ConferenceListType::const_iterator r;
  for (r = conferenceList.begin(); r != conferenceList.end(); ++r) {
    if (roomToCreate == r->second->GetNumber()) {
      conferenceID = r->second->GetID();
      break;
    }
  }

  return MakeAndLockConference(conferenceID, roomToCreate, name);
}


Conference * ConferenceManager::MakeAndLockConference(const OpalGloballyUniqueID & conferenceID, 
                                                                   const PString & roomToCreate, 
                                                                   const PString & name)
{
  conferenceListMutex.Wait();

  Conference * conference = NULL;
  BOOL newConference = FALSE;
  ConferenceListType::const_iterator r = conferenceList.find(conferenceID);
  if (r != conferenceList.end())
    conference = r->second;
  else {
    // create the conference
    conference = CreateConference(conferenceID, roomToCreate, name, mcuNumberMap.GetNumber(conferenceID));

    // insert conference into the map
    conferenceList.insert(ConferenceListType::value_type(conferenceID, conference));

    // set the conference count
    maxConferenceCount = PMAX(maxConferenceCount, (PINDEX)conferenceList.size());
    newConference = TRUE;
  }

  if (newConference)
    OnCreateConference(conference);

  return conference;
}

void ConferenceManager::OnCreateConference(Conference * conference)
{
  // set time limit, if there is one
  int timeLimit = OpenMCU::Current().GetRoomTimeLimit();
  if (timeLimit > 0)
    monitor->AddMonitorEvent(new ConferenceTimeLimitInfo(conference->GetID(), PTime() + timeLimit*1000));

  // add file recorder member    
#if ENABLE_TEST_ROOMS
  if((conference->GetNumber().Left(8) == "testroom") && (conference->GetNumber().GetLength() > 8)) return;
#endif
#if ENABLE_ECHO_MIXER
  if(conference->GetNumber().Left(4)*="echo") return;
#endif
  conference->fileRecorder = new ConferenceFileMember(conference, (const PString) "recorder" , PFile::WriteOnly);

  if(conference->autoRecord)
  {
    conference->externalRecorder = new ExternalVideoRecorderThread(conference->GetNumber());
    OpenMCU::Current().HttpWriteEventRoom("Video recording started",conference->GetNumber());
  }

  if(conference->GetNumber() == "testroom") return;

  if(!OpenMCU::Current().GetForceScreenSplit())
  { PTRACE(1,"Conference\tOnCreateConference: \"Force split screen video\" unchecked, " << conference->GetNumber() << " skipping members.conf"); return; }

  FILE *membLst;

  // read members.conf into conference->membersConf
  membLst = fopen(PString(SYS_CONFIG_DIR) + PATH_SEPARATOR + "members_" + conference->GetNumber() + ".conf","rt");
  PStringStream membersConf;
  if(membLst!=NULL)
  { char buf [128];
    while(fgets(buf, 128, membLst)!=NULL) membersConf << buf;
    fclose(membLst);
  }
  conference->membersConf=membersConf;
  if(membersConf.Left(1)!="\n") membersConf="\n"+membersConf;

  // recall last template
  if(!OpenMCU::Current().recallRoomTemplate) return;
  PINDEX dp=membersConf.Find("\nLAST_USED ");
  if(dp!=P_MAX_INDEX)
  { PINDEX dp2=membersConf.Find('\n',dp+10);
    if(dp2!=P_MAX_INDEX)
    { PString lastUsedTemplate=membersConf.Mid(dp+11,dp2-dp-11).Trim();
      cout << "Extracting & loading last used template: " << lastUsedTemplate << "\n";
      conference->confTpl=conference->ExtractTemplate(lastUsedTemplate);
      conference->LoadTemplate(conference->confTpl);
      cout << "***" << conference->confTpl;
    }
  }
}

void ConferenceManager::OnDestroyConference(Conference * conference)
{
  PTRACE(2,"MCU\tOnDestroyConference() Cleaning out conference " << conference->GetNumber());

// step 1: stop external video recorder:

  if(conference->externalRecorder != NULL)
  { PTRACE(4,"EVRT\tVideo Recorder is active - stopping now");
    conference->externalRecorder->running=FALSE;
    PThread::Sleep(1000);
    conference->externalRecorder = NULL;
  }

// step 2: get the copy of memberList (because we will destroy the original):

  conference->GetMutex().Wait();
  Conference::MemberList theCopy(conference->GetMemberList());
  conference->GetMutex().Signal();

// step 3: disconnect remote endpoints:

  if(theCopy.size() > 0)
  for(Conference::MemberList::iterator r=theCopy.begin(); r!=theCopy.end(); ++r)
  {
    Conference::MemberList::iterator s; ConferenceMember * member;
    conference->GetMutex().Wait();
    if((s=conference->GetMemberList().find(r->first)) != conference->GetMemberList().end()) member=s->second;
    else member=NULL; // NULL may be set here or (!) inerator may point to NULL so please keep in mind...
    if(member!=NULL)
    {
      PString name=member->GetName();
      BOOL needsClose = !((name == "cache") || (name=="file recorder"));
      conference->GetMutex().Signal();
      member->SetConference(NULL); // prevent further attempts to read audio/video data from conference

      if(needsClose)
      {
        member->Close();
        r->second = NULL; // don't touch when will find caches
      }
    }
    else conference->GetMutex().Signal();
  }

// step 3.5: additinal check (linphone fails without it)
  PTRACE(3,"MCU\tOnDestroyConference() waiting for visibleMembersCount==0, up to 10 s");
  for(PINDEX i=0;i<100;i++) if(conference->GetVisibleMemberCount()==0) break; else PThread::Sleep(100);

// step 4: delete caches and file recorder:

  if(theCopy.size() > 0)
  for(Conference::MemberList::iterator r=theCopy.begin(); r!=theCopy.end(); ++r)
  if(r->second != NULL)
  {
    if(r->second->GetName() == "cache")
    {
      delete (ConferenceFileMember *)r->second; r->second=NULL;
      conference->GetMemberList().erase(r->first);
    }
    else 
    if(r->second->GetName() == "file recorder")
    {
      if(conference->fileRecorder != NULL)
      {
        delete conference->fileRecorder;
        conference->fileRecorder=NULL;
      }
      else
      {
        delete (ConferenceFileMember *)r->second;
        r->second=NULL;
      }
      conference->GetMemberList().erase(r->first);
    }
  }

// step 5: all done. wait for empty member list (but not as long):

  PTRACE(3,"MCU\tOnDestroyConference() Removal in progress. Waiting up to 10 s");

  for(PINDEX i=0;i<100;i++) if(conference->GetMemberList().size()==0) break; else PThread::Sleep(100);
  PTRACE(3,"MCU\tOnDestroyConference() Removal done");
}


Conference * ConferenceManager::CreateConference(const OpalGloballyUniqueID & _guid,
                                                              const PString & _number,
                                                              const PString & _name,
                                                                          int _mcuNumber)
{ 
#if OPENMCU_VIDEO
#  if ENABLE_ECHO_MIXER
     if (_number.Left(4) *= "echo") return new Conference(*this, _guid, "echo"+_guid.AsString(), _name, _mcuNumber, new EchoVideoMixer());
#  endif
#  if ENABLE_TEST_ROOMS
     if (_number.Left(8) == "testroom")
     { PString number = _number; int count = 0;
       if (_number.GetLength() > 8)
       { count = _number.Mid(8).AsInteger(); if (count <= 0) { count = 0; number = "testroom"; } }
       if (count >= 0) return new Conference(*this, _guid, number, _name, _mcuNumber, new TestVideoMixer(count));
     }
#  endif
#endif

  if(!OpenMCU::Current().GetForceScreenSplit()) return new Conference(*this, _guid, _number, _name, _mcuNumber, NULL);

  PINDEX slashPos = _number.Find('/');
  PString number;
  if (slashPos != P_MAX_INDEX) number=_number.Left(slashPos);
  else number=_number;

  return new Conference(*this, _guid, number, _name, _mcuNumber
#if OPENMCU_VIDEO
                        ,OpenMCU::Current().CreateVideoMixer()
#endif
                        ); 
}

BOOL ConferenceManager::HasConference(const OpalGloballyUniqueID & conferenceID, PString & number)
{
  PWaitAndSignal m(conferenceListMutex);
  ConferenceListType::const_iterator r = conferenceList.find(conferenceID);
  if (r == conferenceList.end())
    return FALSE;
  number = r->second->GetNumber();
  return TRUE;
}

BOOL ConferenceManager::HasConference(const PString & number, OpalGloballyUniqueID & conferenceID)
{
  PWaitAndSignal m(conferenceListMutex);
  ConferenceListType::const_iterator r;
  for (r = conferenceList.begin(); r != conferenceList.end(); ++r) {
    if (r->second->GetNumber() == number) {
      conferenceID = r->second->GetID();
      return TRUE;
    }
  }
  return FALSE;
}

void ConferenceManager::RemoveConference(const OpalGloballyUniqueID & confId)
{
  PWaitAndSignal m(conferenceListMutex);
  ConferenceListType::iterator r = conferenceList.find(confId);
  if (r != conferenceList.end())  {
    Conference * conf = r->second;
    OnDestroyConference(conf);
    conferenceList.erase(confId);
    mcuNumberMap.RemoveNumber(conf->GetMCUNumber());
    monitor->RemoveForConference(conf->GetID());
    cout << "RemoveConference\n";
    delete conf;
  }
}

void ConferenceManager::RemoveMember(const OpalGloballyUniqueID & confId, ConferenceMember * toRemove)
{
  PWaitAndSignal m(conferenceListMutex);
  Conference * conf = toRemove->GetConference();

  OpalGloballyUniqueID id = conf->GetID();  // make a copy of the ID because it may be about to disappear

  delete toRemove;
#if ENABLE_TEST_ROOMS
  if(conf->GetNumber().Left(8) == "testroom") if(!conf->GetVisibleMemberCount()) RemoveConference(id);
#endif
#if ENABLE_ECHO_MIXER
  if(conf->GetNumber().Left(4) *= "echo") RemoveConference(id);
#endif
  if(conf->autoDelete) if(!conf->GetVisibleMemberCount()) RemoveConference(id);
}

void ConferenceManager::AddMonitorEvent(ConferenceMonitorInfo * info)
{
  monitor->AddMonitorEvent(info);
}

///////////////////////////////////////////////////////////////

void ConferenceMonitor::Main()
{
  running = TRUE;

  for (;;) {

    if (!running)
      break;

    Sleep(1000);

    if (!running)
      break;

    PWaitAndSignal m(mutex);

    PTime now;
    MonitorInfoList::iterator r = monitorList.begin();
    while (r != monitorList.end()) {
      ConferenceMonitorInfo & info = **r;
      if (now < info.timeToPerform)
        ++r;
      else {
        BOOL deleteAfterPerform = TRUE;
        {
          PWaitAndSignal m2(manager.GetConferenceListMutex());
          ConferenceListType & confList = manager.GetConferenceList();
          ConferenceListType::iterator s = confList.find(info.guid);
          if (s != confList.end())
            deleteAfterPerform = info.Perform(*s->second);
        }
        if (!deleteAfterPerform)
          ++r;
        else {
          delete *r;
          monitorList.erase(r);
          r = monitorList.begin();
        }
      }
    }
  }
}

void ConferenceMonitor::AddMonitorEvent(ConferenceMonitorInfo * info)
{
  PWaitAndSignal m(mutex);
  monitorList.push_back(info);
}

void ConferenceMonitor::RemoveForConference(const OpalGloballyUniqueID & guid)
{
  PWaitAndSignal m(mutex);

  MonitorInfoList::iterator r = monitorList.begin();
  while (r != monitorList.end()) {
    ConferenceMonitorInfo & info = **r;
    if (info.guid != guid)
      ++r;
    else {
      delete *r;
      monitorList.erase(r);
      r = monitorList.begin();
    }
  }
}

BOOL ConferenceTimeLimitInfo::Perform(Conference & conference)
{
  Conference::MemberList & list = conference.GetMemberList();
  Conference::MemberList::iterator r;
  for (r = list.begin(); r != list.end(); ++r)
    r->second->Close();
  return TRUE;
}

BOOL ConferenceRepeatingInfo::Perform(Conference & conference)
{
  this->timeToPerform = PTime() + repeatTime;
  return FALSE;
}


BOOL ConferenceMCUCheckInfo::Perform(Conference & conference)
{
  // see if any member of this conference is a not an MCU
  Conference::MemberList & list = conference.GetMemberList();
  Conference::MemberList::iterator r;
  for (r = list.begin(); r != list.end(); ++r)
    if (!r->second->IsMCU())
      break;

  // if there is any non-MCU member, check again later
  if (r != list.end())
    return ConferenceRepeatingInfo::Perform(conference);

  // else shut down the conference
  for (r = list.begin(); r != list.end(); ++r)
    r->second->Close();

  return TRUE;
}


////////////////////////////////////////////////////////////////////////////////////

Conference::Conference(        ConferenceManager & _manager,
                       const OpalGloballyUniqueID & _guid,
                                    const PString & _number,
                                    const PString & _name,
                                                int _mcuNumber
#if OPENMCU_VIDEO
                                    ,MCUVideoMixer * _videoMixer
#endif
)
  : manager(_manager), guid(_guid), number(_number), name(_name), mcuNumber(_mcuNumber), mcuMonitorRunning(FALSE)
{ 
#if OPENMCU_VIDEO
  VMLInit(_videoMixer);
#endif
  maxMemberCount = 0;
  moderated = FALSE;
  muteUnvisible = FALSE;
  VAdelay = 1000;
  VAtimeout = 10000;
  VAlevel = 100;
  echoLevel = 0;
  vidmembernum = 0;
  fileRecorder = NULL;
  externalRecorder=NULL;
  autoDelete=FALSE;
  autoRecord=FALSE;
  PTRACE(3, "Conference\tNew conference started: ID=" << guid << ", number = " << number);
}

Conference::~Conference()
{
#if OPENMCU_VIDEO
  VMLClear();
#endif
}

int Conference::GetVisibleMemberCount() const
{
  PWaitAndSignal m(memberListMutex);
  int visibleMembers = 0;
  std::map<void *, ConferenceMember *>::const_iterator r;
  for (r = memberList.begin(); r != memberList.end(); r++) {
    if (r->second->IsVisible())
      ++visibleMembers;
  }
  return visibleMembers;
}

void Conference::AddMonitorEvent(ConferenceMonitorInfo * info)
{ 
  manager.AddMonitorEvent(info); 
}

BOOL Conference::InviteMember(const char *membName, void * userData)
{
  char buf[128];
  int i=strlen(membName)-1;
  if(membName[i]!=']')
  {
    i=0;
  }
  else
  {
    while(i>=0 && membName[i]!='[') i--;
    if(i<0) return FALSE;
    i++;
  }
  sscanf(&membName[i],"%127[^]]",buf); //buf[strlen(buf)-1]=0;
  PString address = buf;

  OpenMCUH323EndPoint & ep = OpenMCU::Current().GetEndpoint();

  if(!OpenMCU::Current().AreLoopbackCallsAllowed())
  {
    H323TransportAddressArray taa = ep.GetInterfaceAddresses(TRUE, NULL); // todo: join with SIP listener(s) ?
    for(PINDEX i=0; i<taa.GetSize(); i++)
    {
      if(taa[i].Find("ip$"+address+":") == 0)
      {
        PTRACE(6,"Conference\tInviteMember Loopback call rejected (" << taa[i] << "): " << membName << " -> address=" << address);
        return FALSE;
      }
    }
  }

  if(address.Left(4) == "sip:")
  {
    PStringStream msg;
    msg << "Inviting " << address;
    OpenMCU::Current().HttpWriteEventRoom(msg,number);
    if(userData!=NULL)
    {
      OpenMCU::Current().sipendpoint->sipCallData.InsertAt(0, new PString(*(PString *)userData+","+address));
      delete (PString *) userData;
    } else {
      OpenMCU::Current().sipendpoint->sipCallData.InsertAt(0, new PString(number+","+address));
    }
  }
  else // H.323
  {
    if(address.Left(5) == "h323:") address = address.Right(address.GetLength()-5);
    PString port = address.Tokenise(":")[1];
    if(port == "")
    {
      port = OpenMCU::Current().GetEndpointParamFromUri("H323 port", address, "h323");
      if(port != "") address += ":"+port;
    }
    PString h323Token;
    if(userData == NULL) userData = new PString(number);
    PStringStream msg;
    msg << "Inviting " << address;
    OpenMCU::Current().HttpWriteEventRoom(msg,number);
    if (ep.MakeCall(address, h323Token, userData) == NULL)
    {
      PTRACE(6,"Conference\tInvite error, address: " << address);
      cout << "Invite error: " << address << "\n";
      return FALSE;
    }
  }
  return TRUE;
}

BOOL Conference::AddMember(ConferenceMember * memberToAdd)
{

  PTRACE(3, "Conference\tAbout to add member " << memberToAdd->GetTitle() << " to conference " << guid);

  // see if the callback refuses the new member (always true)
  if (!BeforeMemberJoining(memberToAdd))
    return FALSE;

  memberToAdd->SetName();

  // check for duplicate name or very fast reconnect
  {
    Conference::MemberNameList::const_iterator s = memberNameList.find(memberToAdd->GetName());
    if(MCUConfig("Parameters").GetBoolean(RejectDuplicateNameKey, TRUE))
    {
      if(s != memberNameList.end())
      {
        if(s->second != NULL)
        {
          PString username=memberToAdd->GetName(); username.Replace("&","&amp;",TRUE,0); username.Replace("\"","&quot;",TRUE,0);
          PStringStream msg;
          msg << username << " REJECTED - DUPLICATE NAME";
          OpenMCU::Current().HttpWriteEventRoom(msg, number);
          return FALSE;
        }
      }
    } else {
      while (s != memberNameList.end())
      {
        if(s->second == NULL) break;
        {
          PString username=memberToAdd->GetName();
          PString newName=username;
          PINDEX hashPos = newName.Find(" ##");
          if(hashPos == P_MAX_INDEX) newName += " ##2";
          else newName = newName.Left(hashPos+3) + PString(newName.Mid(hashPos+3).AsInteger() + 1);
          memberToAdd->SetName(newName);
          s = memberNameList.find(memberToAdd->GetName());

          username.Replace("&","&amp;",TRUE,0); username.Replace("\"","&quot;",TRUE,0);
          PStringStream msg;
          msg << username << " DUPLICATE NAME, renamed to " << newName;
          OpenMCU::Current().HttpWriteEventRoom(msg, number);
        }
      }
    }
  }

  // add the member to the conference
  if (!memberToAdd->AddToConference(this))
    return FALSE;

  {
    PTRACE(3, "Conference\tAdding member " << memberToAdd->GetName() << " " << memberToAdd->GetTitle() << " to conference " << guid);
    cout << "Adding member " << memberToAdd->GetName() << " " << memberToAdd->GetTitle() << " to conference " << guid << endl;

    // lock the member list
    PWaitAndSignal m(memberListMutex);
    std::map<void *, ConferenceMember *>::const_iterator r;

    ConferenceMemberId mid = memberToAdd->GetID();

    r = memberList.find(mid);
    if(r != memberList.end()) return FALSE;

#if OPENMCU_VIDEO
//    if(!UseSameVideoForAllMembers()) memberToAdd->videoStatus = 1;

    if (moderated==FALSE
#  if ENABLE_TEST_ROOMS
      || number=="testroom"
#  endif
    )
    {
     if (UseSameVideoForAllMembers() && memberToAdd->IsVisible()) {
      if (!videoMixerList->mixer->AddVideoSource(mid, *memberToAdd)) 
        memberToAdd->SetFreezeVideo(TRUE);
       //        memberToAdd->videoStatus = 1;
     PTRACE(3, "Conference\tUseSameVideoForAllMembers ");
     }
    }
    else memberToAdd->SetFreezeVideo(TRUE);

#endif

    // add this member to the conference member list
    memberList.insert(MemberList::value_type(memberToAdd->GetID(), memberToAdd));

    int tid = terminalNumberMap.GetNumber(memberToAdd->GetID());
    memberToAdd->SetTerminalNumber(tid);

    // make sure each member has a connection created for the new member
    // make sure the new member has a connection created for each existing member
    PINDEX visibleMembers = 0;
//    std::map<void *, ConferenceMember *>::const_iterator r;
    for (r = memberList.begin(); r != memberList.end(); r++) {
      ConferenceMember * conn = r->second;
      if (conn != memberToAdd) {
        conn->AddConnection(memberToAdd);
        memberToAdd->AddConnection(conn);
#if OPENMCU_VIDEO
        if (moderated==FALSE
#  if ENABLE_TEST_ROOMS
         || number == "testroom"
#  endif
        )
        if (!UseSameVideoForAllMembers()) {
          if (conn->IsVisible())
          {
            memberToAdd->AddVideoSource(conn->GetID(), *conn);
          }
          if (memberToAdd->IsVisible())
          {
            conn->AddVideoSource(memberToAdd->GetID(), *memberToAdd);
          }
        }
#endif
      }
      if (conn->IsVisible())
        ++visibleMembers;
    }

    // update the statistics
    if (memberToAdd->IsVisible()) {
      maxMemberCount = PMAX(maxMemberCount, visibleMembers);

      // trigger H245 thread for join message
//      new NotifyH245Thread(*this, TRUE, memberToAdd);
    }
  }

  // notify that member is joined
  memberToAdd->SetJoined(TRUE);

  // call the callback function
  OnMemberJoining(memberToAdd);

  if (memberToAdd->IsMCU() && !mcuMonitorRunning) {
    manager.AddMonitorEvent(new ConferenceMCUCheckInfo(GetID(), 1000));
    mcuMonitorRunning = TRUE;
  }

  PStringStream msg;

  // add this member to the conference member name list
  if(memberToAdd!=memberToAdd->GetID())
  {
    if(memberToAdd->GetName().Find(" ##") == P_MAX_INDEX)
    {
      // поиск по UrlId
      BOOL found = FALSE;
      for (Conference::MemberNameList::const_iterator s = memberNameList.begin(); s != memberNameList.end(); ++s)
      {
        PString memberName = s->first;
        if(memberName.Find(memberToAdd->GetUrlId()) != P_MAX_INDEX)
        {
          if(s->second == NULL)
          {
            memberNameList.erase(memberName);
            if(!found) confTpl.Replace(memberName,memberToAdd->GetName(),TRUE,0);
            found = TRUE;
          }
        }
      }
    } else {
      memberNameList.erase(memberToAdd->GetName());
    }
    memberNameList.insert(MemberNameList::value_type(memberToAdd->GetName(),memberToAdd));

    PullMemberOptionsFromTemplate(memberToAdd, confTpl);

    PString username=memberToAdd->GetName(); username.Replace("&","&amp;",TRUE,0); username.Replace("\"","&quot;",TRUE,0);
    msg="addmmbr(1,"; msg << (long)memberToAdd->GetID()
     << ",\"" << username << "\"," << memberToAdd->muteIncoming
     << "," << memberToAdd->disableVAD << ","
     << memberToAdd->chosenVan << ","
     << memberToAdd->GetAudioLevel() << ","
     << memberToAdd->GetVideoMixerNumber() << ")";
    OpenMCU::Current().HttpWriteCmdRoom(msg,number);
  }
/*  
  else
  {
   serviceMemberNameList.erase(memberToAdd->GetName());
   serviceMemberNameList.insert(MemberNameList::value_type(memberToAdd->GetName(),memberToAdd));
  }
*/  


  msg = "<font color=green><b>+</b>";
  msg << memberToAdd->GetName() << "</font>"; OpenMCU::Current().HttpWriteEventRoom(msg,number);
  return TRUE;
}


BOOL Conference::RemoveMember(ConferenceMember * memberToRemove)
{
  if(memberToRemove == NULL) return TRUE;

  PWaitAndSignal m(memberListMutex);

  PString username = memberToRemove->GetName();

  if(!memberToRemove->IsJoined())
  {
    PTRACE(4, "Conference\tNo need to remove call " << username << " from conference " << guid);
    return (memberList.size() == 0);
  }

  ConferenceMemberId userid = memberToRemove->GetID();

  // add this member to the conference member name list with zero id

  PTRACE(3, "Conference\tRemoving call " << username << " from conference " << guid << " with size " << (PINDEX)memberList.size());
  cout << username << " leaving conference " << number << "(" << guid << ")" << endl;


  BOOL closeConference;
  {

    MemberNameList::iterator s;
    s = memberNameList.find(username);
    
    ConferenceMember *zerop=NULL;
    if(memberToRemove!=userid && s->second==memberToRemove && s!=memberNameList.end())
     {
       memberNameList.erase(username);
       memberNameList.insert(MemberNameList::value_type(username,zerop));
     }

    PStringStream msg; msg << "<font color=red><b>-</b>" << username << "</font>"; OpenMCU::Current().HttpWriteEventRoom(msg,number);
    username.Replace("&","&amp;",TRUE,0); username.Replace("\"","&quot;",TRUE,0);
    msg="remmmbr(0,"; msg << (long)userid
     << ",\"" << username << "\"," << memberToRemove->muteIncoming
     << "," << memberToRemove->disableVAD << ","
     << memberToRemove->chosenVan << ","
     << memberToRemove->GetAudioLevel();
    if(s==memberNameList.end()) msg << ",1";
    msg << ")";
    OpenMCU::Current().HttpWriteCmdRoom(msg,number);


    // remove this connection from the member list
    memberList.erase(userid);
    memberToRemove->RemoveAllConnections();

    MemberList::iterator r;
    // remove this member from the connection lists for all other members
    for (r = memberList.begin(); r != memberList.end(); r++) {
      ConferenceMember * conn = r->second;
      if(conn != NULL)
      if (conn != memberToRemove) {
        conn->RemoveConnection(userid);
#if OPENMCU_VIDEO
        if (!UseSameVideoForAllMembers()) {
          if (memberToRemove->IsVisible())
            conn->RemoveVideoSource(userid, *memberToRemove);
          if (conn->IsVisible())
            memberToRemove->RemoveVideoSource(conn->GetID(), *conn);
        }
#endif
      }
    }

#if OPENMCU_VIDEO
    if (moderated==FALSE
#  if ENABLE_TEST_ROOMS
    || number == "testroom"
#  endif
    )
    { if (UseSameVideoForAllMembers())
      if (memberToRemove->IsVisible())
        videoMixerList->mixer->RemoveVideoSource(userid, *memberToRemove);
    }
    else
    {
      VideoMixerRecord * vmr=videoMixerList; while(vmr!=NULL)
      {
        vmr->mixer->MyRemoveVideoSourceById(userid,FALSE);
        vmr=vmr->next;
      }
    }
#endif

    // trigger H245 thread for leave message
//    if (memberToRemove->IsVisible())
//      new NotifyH245Thread(*this, FALSE, memberToRemove);

    terminalNumberMap.RemoveNumber(memberToRemove->GetTerminalNumber());


    // return TRUE if conference is empty 
//    closeConference = memberList.size() == 0;
    closeConference = GetVisibleMemberCount() == 0;
  }

  // notify that member is not joined anymore
  memberToRemove->SetJoined(FALSE);

  // call the callback function
//  if (!closeConference)
    OnMemberLeaving(memberToRemove);

  return closeConference;
}


void Conference::ReadMemberAudio(ConferenceMember * member, void * buffer, PINDEX amount, unsigned sampleRate, unsigned channels)
{
  // get number of channels to mix
  ConferenceMember::ConnectionListType & connectionList = member->GetConnectionList();
  ConferenceMember::ConnectionListType::iterator r;
  for (r = connectionList.begin(); r != connectionList.end(); ++r) 
    if (r->second != NULL)
    {
      BOOL skip=moderated&&muteUnvisible;
      if(skip)
      {
        VideoMixerRecord * vmr = videoMixerList;
        while(vmr != NULL) if(vmr->mixer->GetPositionStatus(r->first)>=0)
        { skip=FALSE; break; }
        else vmr=vmr->next;
      }
      if(!skip) // default behaviour
        r->second->ReadAndMixAudio((BYTE *)buffer, amount, (PINDEX)connectionList.size(), 0, sampleRate, channels);
    }
}

// tint - time interval since last call in msec
void Conference::WriteMemberAudioLevel(ConferenceMember * member, unsigned audioLevel, int tint)
{
  member->audioLevelIndicator|=audioLevel;
  if(!((++member->audioCounter)&31)){
    if (member->audioLevelIndicator < 64) member->audioLevelIndicator = 0;
    if((member->previousAudioLevel != member->audioLevelIndicator)||((member->audioLevelIndicator!=0) && ((member->audioCounter&255)==0))){
      PStringStream msg; msg << "audio(" << (long)member->GetID() << "," << member->audioLevelIndicator << ")";
      OpenMCU::Current().HttpWriteCmdRoom(msg,number);
      member->previousAudioLevel=member->audioLevelIndicator;
      member->audioLevelIndicator=audioLevel;
    }
  }
#if OPENMCU_VIDEO
  if (UseSameVideoForAllMembers()) {
    if (videoMixerList != NULL)
    {
      if(audioLevel > VAlevel) member->vad+=tint;
      else member->vad=0;
      VideoMixerRecord * vmr=videoMixerList; while(vmr!=NULL)
      {
        MCUVideoMixer * videoMixer = vmr->mixer;
        int status = videoMixer->GetPositionStatus(member->GetID());
        if(audioLevel > VAlevel)
        {
//          cout << "audioLevel " << audioLevel << "\n";
          if(member->vad >= VAdelay) // voice-on trigger delay
          {
//            cout << "VAD=" << member->vad << " status=" << status << "\n";
            if(status > 0) videoMixer->SetPositionStatus(member->GetID(),0);
            else if(status == 0 && member->disableVAD == FALSE)
            {
              if(member->vad-VAdelay>500) // execute every 500 ms of voice activity
                videoMixer->SetVAD2Position(member);
            }
            else if(status == -1 && member->disableVAD == FALSE) //find new vad position for active member
            {
              ConferenceMemberId id = videoMixer->SetVADPosition(member,member->chosenVan,VAtimeout);
              if(id!=NULL)
              { FreezeVideo(id); member->SetFreezeVideo(FALSE); }
            }
          }
        }
        else
        {
          if(status >= 0) // increase silence counter
            videoMixer->SetPositionStatus(member->GetID(),status+tint);
        }
        if(audioLevel > VAlevel) if(status==0 && member->disableVAD==FALSE) if(member->vad-VAdelay>500) member->vad=VAdelay;
        vmr=vmr->next;
      }
    }
  }
#endif // OPENMCU_VIDEO
}


#if OPENMCU_VIDEO

void Conference::ReadMemberVideo(ConferenceMember * member, void * buffer, int width, int height, PINDEX & amount)
{
  if (videoMixerList == NULL)
    return;

// PTRACE(3, "Conference\tReadMemberVideo call 1" << width << "x" << height);
  unsigned mixerNumber; if(member==NULL) mixerNumber=0; else mixerNumber=member->GetVideoMixerNumber();
  MCUVideoMixer * mixer = VMLFind(mixerNumber);

  if(mixer==NULL)
  { if(mixerNumber != 0) mixer = VMLFind(0);
    if(mixer==NULL)
    { PTRACE(3,"Conference\tCould not get video");
      return;
    } else { PTRACE(6,"Conference\tCould not get video mixer " << mixerNumber << ", reading 0 instead"); }
  }

//  if (mixer!=NULL) {
    mixer->ReadFrame(*member, buffer, width, height, amount);
//    return;
//  }

/* commented by kay27 not really understanding what he is doing, 04.09.2013
  // find the other member and copy it's video
  PWaitAndSignal m(memberListMutex);
  MemberList::iterator r;
  for (r = memberList.begin(); r != memberList.end(); r++) {
    if ((r->second != member) && r->second->IsVisible()) {
      void * frameStore = r->second->OnExternalReadVideo(member->GetID(), width, height, amount);  
      if (frameStore != NULL) {
        memcpy(buffer, frameStore, amount);
        r->second->UnlockExternalVideo();
      }
    }
  }
*/
  
}

BOOL Conference::WriteMemberVideo(ConferenceMember * member, const void * buffer, int width, int height, PINDEX amount)
{
  if (UseSameVideoForAllMembers()) {
    if (videoMixerList != NULL) {
      VideoMixerRecord * vmr = videoMixerList; BOOL writeResult=FALSE;
      while(vmr!=NULL)
      { writeResult |= vmr->mixer->WriteFrame(member->GetID(), buffer, width, height, amount);
        vmr=vmr->next;
      }
      return writeResult;
    }
  }
  else {
    PWaitAndSignal m(memberListMutex);
    MemberList::iterator r;
    for (r = memberList.begin(); r != memberList.end(); ++r)
      r->second->OnExternalSendVideo(member->GetID(), buffer, width, height, amount);
  }
  return TRUE;
}

#endif

BOOL Conference::BeforeMemberJoining(ConferenceMember * member)
{ 
  return manager.BeforeMemberJoining(this, member); 
}

void Conference::OnMemberJoining(ConferenceMember * member)
{ 
  manager.OnMemberJoining(this, member); 
}

void Conference::OnMemberLeaving(ConferenceMember * member)
{ 
  manager.OnMemberLeaving(this, member); 
}

void Conference::FreezeVideo(ConferenceMemberId id)
{ 
  int i;
  PWaitAndSignal m(memberListMutex);
  MemberList::iterator r,s;
  if(id!=NULL)
  {
    r = memberList.find(id); if(r == memberList.end()) return;
    VideoMixerRecord * vmr = videoMixerList; while(vmr!=NULL)
    { i=vmr->mixer->GetPositionStatus(id);
      if(i>=0) {
        r->second->SetFreezeVideo(FALSE);
        return;
      }
      vmr=vmr->next;
    }
    if(!UseSameVideoForAllMembers())for(s=memberList.begin();s!=memberList.end();++s) if(s->second->videoMixer!=NULL) if(s->second->videoMixer->GetPositionStatus(id)>=0) {r->second->SetFreezeVideo(FALSE); return;}
    r->second->SetFreezeVideo(TRUE);
    return;
  }
  for (r = memberList.begin(); r != memberList.end(); r++) {
    ConferenceMemberId mid=r->second->GetID();
    VideoMixerRecord * vmr = videoMixerList; while(vmr!=NULL){
      i=vmr->mixer->GetPositionStatus(mid);
      if(i>=0) {
        r->second->SetFreezeVideo(FALSE);
        break;
      }
      vmr=vmr->next;
    }
    if(vmr==NULL)
    { if(!UseSameVideoForAllMembers())
      { for(s=memberList.begin();s!=memberList.end();++s)
        { if(s->second->videoMixer!=NULL) if(s->second->videoMixer->GetPositionStatus(mid)>=0)
          { r->second->SetFreezeVideo(FALSE);
            break;
          }
        }
        if(s==memberList.end()) r->second->SetFreezeVideo(TRUE);
      }
      else r->second->SetFreezeVideo(TRUE);
    }
  }
}

BOOL Conference::PutChosenVan()
{
  BOOL put=FALSE;
  int i;
  PWaitAndSignal m(memberListMutex);
  MemberList::iterator r;
  for (r = memberList.begin(); r != memberList.end(); ++r) {
    if(r->second->chosenVan) {
      VideoMixerRecord * vmr = videoMixerList;
      while(vmr!=NULL){
        i=vmr->mixer->GetPositionStatus(r->second->GetID());
        if(i < 0) put |= (NULL != vmr->mixer->SetVADPosition(r->second,r->second->chosenVan,VAtimeout));
        vmr = vmr->next;
      }
    }
  }
  return put;
}

void Conference::HandleFeatureAccessCode(ConferenceMember & member, PString fac){
  PTRACE(3,"Conference\tHandling feature access code " << fac << " from " << member.GetName());
  PStringArray s = fac.Tokenise("*");
  if(s[0]=="1")
  {
    int posTo=0;
    if(s.GetSize()>1) posTo=s[1].AsInteger();
    PTRACE(4,"Conference\t" << "*1*" << posTo << "#: jump into video position " << posTo);
    if(videoMixerCount==0) return;
    ConferenceMemberId id=member.GetID();
    if(id==NULL) return;
    int pos=videoMixerList->mixer->GetPositionNum(id);
    if(pos==posTo) return;
    videoMixerList->mixer->InsertVideoSource(&member,posTo);
    FreezeVideo(NULL);
    OpenMCU::Current().HttpWriteCmdRoom(OpenMCU::Current().GetEndpoint().GetConferenceOptsJavascript(*this),number);
    OpenMCU::Current().HttpWriteCmdRoom("build_page()",number);
  }
}


///////////////////////////////////////////////////////////////////////////

ConferenceMember::ConferenceMember(Conference * _conference, ConferenceMemberId _id, BOOL _isMCU)
  : conference(_conference), id(_id), isMCU(_isMCU)
{
  audioLevel = 0;
  audioCounter = 0; previousAudioLevel = 65535; audioLevelIndicator = 0;
  terminalNumber = -1;
  memberIsJoined = FALSE;

#if OPENMCU_VIDEO
  videoMixer = NULL;
//  fsConverter = PColourConverter::Create("YUV420P", "YUV420P", CIF4_WIDTH, CIF4_HEIGHT);
//  MCUVideoMixer::FillCIF4YUVFrame(memberFrameStores.GetFrameStore(CIF4_WIDTH, CIF4_HEIGHT).data.GetPointer(), 0, 0, 0);
  totalVideoFramesReceived = 0;
  firstFrameReceiveTime = -1;
  totalVideoFramesSent = 0;
  firstFrameSendTime = -1;
  rxFrameWidth = 0; rxFrameHeight = 0;
  vad = 0;
  autoDial = FALSE;
  muteIncoming = FALSE;
  disableVAD = FALSE;
  chosenVan = 0;
  videoMixerNumber = 0;
#endif
}

ConferenceMember::~ConferenceMember()
{
  muteIncoming = TRUE;
#if OPENMCU_VIDEO
  delete videoMixer;
#endif
}   


BOOL ConferenceMember::AddToConference(Conference * _conference)
{
  //if (conference != NULL)
  //  return FALSE;
  //conference = _conference;
#if OPENMCU_VIDEO
  if (!conference->UseSameVideoForAllMembers())
    videoMixer = new MCUSimpleVideoMixer();
#endif
  return TRUE;
}

void ConferenceMember::RemoveFromConference()
{
  if (conference != NULL) {
    if (conference->RemoveMember(this))
    {
      if(conference->autoDelete
#     if ENABLE_TEST_ROOMS
        || (conference->GetNumber().Left(8) == "testroom")
#     endif
#     if ENABLE_ECHO_MIXER
        || (conference->GetNumber().Left(4) *= "echo")
#     endif
      )
      conference->GetManager().RemoveConference(conference->GetID());
    }
  }
}

void ConferenceMember::AddConnection(ConferenceMember * memberToAdd)
{
  ConferenceMemberId newID = memberToAdd->GetID();
  PTRACE(3, "Conference\tAdding " << newID << " to connection " << id);
  if (lock.Wait(TRUE)) {
    ConferenceConnection * conn = memberToAdd->CreateConnection();
    memberList.insert(MemberListType::value_type(newID, memberToAdd));
    connectionList.insert(ConnectionListType::value_type(newID, conn));
    lock.Signal(TRUE);
  }
}

void ConferenceMember::RemoveConnection(ConferenceMemberId idToDelete)
{
  PTRACE(3, "Conference\tRemoving member " << idToDelete << " from connection " << id);
  if (lock.Wait(TRUE)) {
    memberList.erase(idToDelete);
    connectionList.erase(idToDelete);
    lock.Signal(TRUE);
  }
}

void ConferenceMember::RemoveAllConnections()
{
  PTRACE(3, "Conference\tRemoving all members from connection " << id);
  if (lock.Wait(TRUE)) {
    memberList.clear();
    connectionList.clear();
    for(BufferListType::iterator t0=bufferList.begin(); t0!=bufferList.end(); ++t0)
    if(t0->second != NULL) {
#if USE_SWRESAMPLE
      if(t0->second->swrc != NULL) swr_free(&(t0->second->swrc));
      t0->second->swrc = NULL;
#elif USE_AVRESAMPLE
      if(t0->second->swrc != NULL) avresample_free(&(t0->second->swrc));
      t0->second->swrc = NULL;
#elif USE_LIBSAMPLERATE
      if(t0->second->swrc != NULL) src_delete(t0->second->swrc);
      t0->second->swrc = NULL;
#endif
      delete t0->second; t0->second=NULL;
    }
    PTRACE(3,"Conference\tResampling buffers removed from connection " << id);
    lock.Signal(TRUE);
  }
}

void ConferenceMember::WriteAudio(const void * buffer, PINDEX amount, unsigned sampleRate, unsigned channels)
{
  if(muteIncoming) return;
  // calculate average signal level for this member
  unsigned signalLevel = 0;
  {
    int sum = 0;
    int integr = 0;
    const short * pcm = (short *)buffer;
    const short * end = pcm + (amount / 2);
    while (pcm != end) {
//    cout << *pcm << "\n";
      integr +=*pcm;
      if (*pcm < 0) sum -= *pcm++;
      else          sum += *pcm++;
    }
//  cout << "audioInegr = " << integr << "\n";
    signalLevel = sum/(amount/2);
  }
  audioLevel = ((signalLevel * 2) + audioLevel) / 3;
//  cout << "audioLevel = " << audioLevel << "\n";

  if (lock.Wait())
  { if (conference != NULL) conference->WriteMemberAudioLevel(this, audioLevel, amount/32);

    // reset buffer usage flag
    for(BufferListType::iterator t=bufferList.begin(); t!=bufferList.end(); ++t) if(t->second != NULL) t->second->used=FALSE;

    MemberListType::iterator r;
    for (r = memberList.begin(); r != memberList.end(); ++r)
    { if (r->second != NULL) // member in the list != NULL
      { ConnectionListType::iterator s = r->second->connectionList.find(id);
        if(s == r->second->connectionList.end()) continue; // seems this is really needs for same-time connections
        if(s->second == NULL) continue;                    // one more paranoidal check just in case
        { if(s->second->outgoingSampleRate == sampleRate && s->second->outgoingCodecChannels == channels)
            s->second->Write((BYTE *)buffer, amount); // equal rates
          else // resampler needs here
          { unsigned targetSampleRate = s->second->outgoingSampleRate;
            unsigned targetCodecChannels = s->second->outgoingCodecChannels;
            unsigned bufferKey = (targetCodecChannels<<24)|targetSampleRate;
            PINDEX newAmount = amount * targetCodecChannels * targetSampleRate / sampleRate / channels;
            newAmount-=(newAmount % (channels<<1));
            BufferListType::iterator t = bufferList.find(bufferKey);
            if(t==bufferList.end()) // no buffer found, create
            { ResamplerBufferType * newResamplerBuffer = new ResamplerBufferType();
              newResamplerBuffer->used = FALSE;
#if USE_SWRESAMPLE
              const uint64_t MCU_AV_CH_Layout_Selector[] = {0
                ,AV_CH_LAYOUT_MONO
                ,AV_CH_LAYOUT_STEREO
                ,AV_CH_LAYOUT_2_1
                ,AV_CH_LAYOUT_3POINT1
                ,AV_CH_LAYOUT_5POINT0
                ,AV_CH_LAYOUT_5POINT1
                ,AV_CH_LAYOUT_7POINT0
                ,AV_CH_LAYOUT_7POINT1
              };
              newResamplerBuffer->swrc = NULL;
              newResamplerBuffer->swrc = swr_alloc_set_opts(NULL,
                MCU_AV_CH_Layout_Selector[targetCodecChannels], AV_SAMPLE_FMT_S16, targetSampleRate,
                MCU_AV_CH_Layout_Selector[channels           ], AV_SAMPLE_FMT_S16, sampleRate,       0, NULL);
              swr_init(newResamplerBuffer->swrc);
#elif USE_AVRESAMPLE
              const uint64_t MCU_AV_CH_Layout_Selector[] = {0
                ,AV_CH_LAYOUT_MONO
                ,AV_CH_LAYOUT_STEREO
                ,AV_CH_LAYOUT_2_1
                ,AV_CH_LAYOUT_3POINT1
                ,AV_CH_LAYOUT_5POINT0
                ,AV_CH_LAYOUT_5POINT1
                ,AV_CH_LAYOUT_7POINT0
                ,AV_CH_LAYOUT_7POINT1
              };
              newResamplerBuffer->swrc = NULL;
              newResamplerBuffer->swrc = avresample_alloc_context();
              av_opt_set_int(newResamplerBuffer->swrc, "out_channel_layout", MCU_AV_CH_Layout_Selector[targetCodecChannels], 0);
              av_opt_set_int(newResamplerBuffer->swrc, "out_sample_fmt",     AV_SAMPLE_FMT_S16, 0);
              av_opt_set_int(newResamplerBuffer->swrc, "out_sample_rate",    targetSampleRate, 0);
              av_opt_set_int(newResamplerBuffer->swrc, "in_channel_layout",  MCU_AV_CH_Layout_Selector[channels], 0);
              av_opt_set_int(newResamplerBuffer->swrc, "in_sample_fmt",      AV_SAMPLE_FMT_S16,0);
              av_opt_set_int(newResamplerBuffer->swrc, "in_sample_rate",     sampleRate, 0);
              avresample_open(newResamplerBuffer->swrc);
#elif USE_LIBSAMPLERATE
              newResamplerBuffer->swrc = NULL;
              newResamplerBuffer->swrc = src_new(SRC_LINEAR, 1, NULL);
#endif
              bufferList.insert(BufferListType::value_type(bufferKey, newResamplerBuffer));
              t=bufferList.find(bufferKey);
              if(t==bufferList.end()) { PTRACE(1,"Mixer\tBuffer creation error, s/r=" << targetSampleRate); continue; }
            }
            if(t->second==NULL) continue;
            if(!(t->second->used))
            { if(t->second->data.GetSize() < newAmount) t->second->data.SetSize(newAmount + 16);
              DoResample((BYTE *)buffer, amount, sampleRate, channels, t, newAmount, targetSampleRate, targetCodecChannels);
              t->second->used = TRUE;
            }
            s->second->Write((BYTE *)(t->second->data.GetPointer()), newAmount);
          }
        }
      }
    }

    // delete unused buffers
    BufferListType::iterator t0 = bufferList.begin();
    while(t0 != bufferList.end())
    {
      if(t0->second != NULL) if(!(t0->second->used))
      {
#if USE_SWRESAMPLE
        if(t0->second->swrc != NULL) swr_free(&(t0->second->swrc));
        t0->second->swrc = NULL;
#elif USE_AVRESAMPLE
        if(t0->second->swrc != NULL) avresample_free(&(t0->second->swrc));
        t0->second->swrc = NULL;
#elif USE_LIBSAMPLERATE
        if(t0->second->swrc != NULL) src_delete(t0->second->swrc);
        t0->second->swrc = NULL;
#endif
        delete t0->second;
        t0->second=NULL;
        bufferList.erase(t0);
        t0=bufferList.begin(); // needed at least for msvc++ 2010
                               // (it fixes access violation when thread after last iteration strangely failed still here)
        continue;
      }
      t0++;
    }

    lock.Signal();
  }
}

void ConferenceMember::DoResample(BYTE * src, PINDEX srcBytes, unsigned srcRate, unsigned srcChannels, BufferListType::const_iterator t, PINDEX dstBytes, unsigned dstRate, unsigned dstChannels)
{
#if USE_SWRESAMPLE
  void * to = t->second->data.GetPointer();
  void * from = (void*)src;
  swr_convert(t->second->swrc,
    (uint8_t **)&to,
       (((int)dstBytes)>>1)/dstChannels,
    (const uint8_t **)&from,
       (((int)srcBytes)>>1)/srcChannels
  );
#elif USE_AVRESAMPLE
  void * to = t->second->data.GetPointer();
  void * from = (void*)src;

  int out_samples = (((int)dstBytes)>>1)/dstChannels;
  int out_linesize = (int)dstBytes;
  int in_samples = (((int)srcBytes)>>1)/srcChannels;
  int in_linesize = (int)srcBytes;

  avresample_convert(t->second->swrc, (uint8_t **)&to, out_linesize, out_samples,
                                      (uint8_t **)&from, in_linesize, in_samples);
#elif USE_LIBSAMPLERATE
  SRC_DATA src_data;
  long out_samples = (int)dstBytes/(dstChannels*sizeof(short));
  long in_samples = (int)srcBytes/(srcChannels*sizeof(short));
  float data_out[out_samples*sizeof(float)];
  float data_in[in_samples*sizeof(float)];
  src_short_to_float_array((const short *)src, data_in, in_samples);

  src_data.data_in = data_in;
  src_data.input_frames = in_samples;
  src_data.data_out = data_out;
  src_data.output_frames = out_samples;
  src_data.src_ratio = (double)out_samples/(double)in_samples;

  int err = src_process(t->second->swrc, &src_data);
  if (err)
  {
    PTRACE(1, "libsamplerate error: " << src_strerror(err));
    return;
  }
  src_float_to_short_array(data_out, (short *)t->second->data.GetPointer(), src_data.output_frames_gen);
  //PTRACE(1, "libsamplerate: " << src_data.input_frames << " " << src_data.output_frames << " " << src_data.input_frames_used << " " << src_data.output_frames_gen);
#else
  if(srcChannels == dstChannels && srcChannels == 1)
  { for(PINDEX i=0;i<(dstBytes>>1);i++) ((short*)(t->second->data.GetPointer()))[i] = ((short*)src)[i*srcRate/dstRate];
    return;
  }
  if(srcChannels == dstChannels)
  { for(unsigned i=0;i<((dstBytes>>1)/dstChannels);i++)
    { unsigned ofs=(i*srcRate/dstRate)*srcChannels;
      for(unsigned j=0;j<srcChannels;j++) ((short*)(t->second->data.GetPointer()))[i*srcChannels+j] = ((short*)src)[ofs+j];
    }
    return;
  }
  for(unsigned i=0;i<(dstBytes>>1)/dstChannels;i++)
  { unsigned ofs=(i*srcRate/dstRate)*srcChannels, srcChan=0;
    for(unsigned j=0;j<dstChannels;j++)
    { ((short*)(t->second->data.GetPointer()))[i*dstChannels+j] = ((short*)src)[ofs+srcChan];
      srcChan++; if(srcChan>=srcChannels) srcChan=0;
    }
  }
#endif
}

void ConferenceMember::OnExternalSendAudio(ConferenceMemberId source, const void * buffer, PINDEX amount, unsigned sampleRate)
{
  if (lock.Wait()) {
    ConnectionListType::iterator r = connectionList.find(source);
    if (r != connectionList.end())
      if (r->second != NULL)
        r->second->Write((BYTE *)buffer, amount);
    lock.Signal();
  }
}

void ConferenceMember::ReadAudio(void * buffer, PINDEX amount, unsigned sampleRate, unsigned channels)
{
  // First, set the buffer to empty.
  memset(buffer, 0, amount);

  if (lock.Wait()) {
    if (conference != NULL)
      conference->ReadMemberAudio(this, buffer, amount, sampleRate, channels);
    lock.Signal();
  }
}

#if OPENMCU_VIDEO

// called whenever the connection needs a frame of video to send
void ConferenceMember::ReadVideo(void * buffer, int width, int height, PINDEX & amount)
{
  ++totalVideoFramesSent;
  if (!firstFrameSendTime.IsValid())
    firstFrameSendTime = PTime();
  if (lock.Wait()) {
    if (conference != NULL) {
      if (conference->UseSameVideoForAllMembers())
        conference->ReadMemberVideo(this, buffer, width, height, amount);
      else if (videoMixer != NULL)
        videoMixer->ReadFrame(*this, buffer, width, height, amount);
    }
    lock.Signal();
  }
}

// called whenever the connection receives a frame of video
void ConferenceMember::WriteVideo(const void * buffer, int width, int height, PINDEX amount)
{
  ++totalVideoFramesReceived;
  rxFrameWidth=width; rxFrameHeight=height;
  if (!firstFrameReceiveTime.IsValid())
    firstFrameReceiveTime = PTime();

  if (lock.Wait()) {
    if (conference != NULL) {
      if (!conference->WriteMemberVideo(this, buffer, width, height, amount)) {
        PWaitAndSignal m(memberFrameStoreMutex);
        VideoFrameStoreList::FrameStore & fs = memberFrameStores.GetFrameStore(width, height);
        memcpy(fs.data.GetPointer(), buffer, amount);
        memberFrameStores.InvalidateExcept(width, height);
      }
    }
    lock.Signal();
  }
}

void ConferenceMember::OnExternalSendVideo(ConferenceMemberId id, const void * buffer, int width, int height, PINDEX amount)
{
//  if (lock.Wait()) {
    videoMixer->WriteFrame(id, buffer, width, height, amount);
//    lock.Signal();
//  }
}

void * ConferenceMember::OnExternalReadVideo(ConferenceMemberId id, int width, int height, PINDEX & bytesReturned)
{
  if (!lock.Wait())
    return NULL;

  memberFrameStoreMutex.Wait();

  BOOL found;
  VideoFrameStoreList::FrameStore & nearestFs = memberFrameStores.GetNearestFrameStore(width, height, found);

  // if no valid framestores, nothing we can do
/*  if (!found) {
    memberFrameStoreMutex.Signal();
    lock.Signal();
    return NULL;
  }
*/
  // if the valid framestore is a perfect match, return it
  if(found)
  if ((nearestFs.width == width) && (nearestFs.height == height))
    return nearestFs.data.GetPointer();

  // create a new destinationf framestore
  VideoFrameStoreList::FrameStore & destFs = memberFrameStores.GetFrameStore(width, height);

  if(found)
  MCUVideoMixer::ResizeYUV420P(nearestFs.data.GetPointer(), destFs.data.GetPointer(), nearestFs.width, nearestFs.height, width, height);
  else
//  OpenMCU::Current().GetPreMediaFrame(destFs.data.GetPointer(), width, height, bytesReturned);
  MCUVideoMixer::FillYUVFrame(destFs.data.GetPointer(), 0, 0, 0, width, height);
  destFs.valid = TRUE;

  return destFs.data.GetPointer();
}

void ConferenceMember::UnlockExternalVideo()
{ 
  memberFrameStoreMutex.Signal(); 
  lock.Signal();
}

BOOL ConferenceMember::AddVideoSource(ConferenceMemberId id, ConferenceMember & mbr)
{
  PAssert(videoMixer != NULL, "attempt to add video source to NULL video mixer");
  return videoMixer->AddVideoSource(id, mbr);
}

void ConferenceMember::RemoveVideoSource(ConferenceMemberId id, ConferenceMember & mbr)
{
  PAssert(videoMixer != NULL, "attempt to remove video source from NULL video mixer");
  videoMixer->RemoveVideoSource(id, mbr);
}

BOOL ConferenceMember::OnOutgoingVideo(void * buffer, int width, int height, PINDEX & amount)
{
  return FALSE;
}

BOOL ConferenceMember::OnIncomingVideo(const void * buffer, int width, int height, PINDEX amount)
{
  return FALSE;
}

#endif

PString ConferenceMember::GetMonitorInfo(const PString & /*hdr*/)
{ 
  return PString::Empty(); 
}

///////////////////////////////////////////////////////////////////////////

ConferenceConnection::ConferenceConnection(ConferenceMemberId _id)
//  : id(_id), bufferSize(PCM_BUFFER_SIZE)
  : id(_id)
{
  outgoingSampleRate = 8000;
  outgoingCodecChannels = 1;
  PCM_BUFFER_SIZE_CALC(outgoingSampleRate,outgoingCodecChannels);
  bufferStart = bufferLen = 0;
  hasUnderflow = TRUE; // we would like to accumulate some data before we'll hear it :)
}

ConferenceConnection::~ConferenceConnection()
{
//  delete[] buffer;
}

void ConferenceConnection::Write(const BYTE * data, PINDEX amount)
{
  if (amount == 0) return;

  PWaitAndSignal mutex(audioBufferMutex);
  
  // if there is not enough room for the new data, make room
  PINDEX newLen = bufferLen + amount;
  if (newLen > bufferSize) {
    PINDEX toRemove = newLen - bufferSize;
    bufferStart = (bufferStart + toRemove) % bufferSize;
    bufferLen -= toRemove;
  }

  // copy data to the end of the new data, up to the end of the buffer
  PINDEX copyStart = (bufferStart + bufferLen) % bufferSize;
  if ((copyStart + amount) > bufferSize) {
    PINDEX toCopy = bufferSize - copyStart;
    memcpy((BYTE *)((unsigned long)buffer.GetPointer() + copyStart), data, toCopy);
    copyStart = 0;
    data      += toCopy;
    amount    -= toCopy;
    bufferLen += toCopy;
  }

  // copy the rest of the data
  if (amount > 0) {
    memcpy((BYTE *)((unsigned long)buffer.GetPointer() + copyStart), data, amount);
    bufferLen   += amount;
  }
}


void ConferenceConnection::ReadAudio(BYTE * data, PINDEX amount)
{
  if (amount == 0) return;

  PWaitAndSignal mutex(audioBufferMutex);
  
  if (bufferLen == 0) {
    memset(data, 0, amount); // nothing in the buffer. return silence
    return;
  }

  // fill output data block with silence if audiobuffer is
  // almost empty.
  if (amount > bufferLen) 
    memset(data + bufferLen, 0, amount - bufferLen);

  // only copy up to the amount of data remaining
  PINDEX copyLeft = PMIN(amount, bufferLen);

  // if buffer is wrapping, get first part
  if ((bufferStart + copyLeft) > bufferSize) {
    PINDEX toCopy = bufferSize - bufferStart;

    memcpy(data, (BYTE *)((unsigned long)buffer.GetPointer() + bufferStart), toCopy);

    data        += toCopy;
    bufferLen   -= toCopy;
    copyLeft    -= toCopy;
    bufferStart = 0;
  }

  // get the remainder of the buffer
  if (copyLeft > 0) {

    memcpy(data, (BYTE *)((unsigned long)buffer.GetPointer() + bufferStart), copyLeft);

    bufferLen -= copyLeft;
    bufferStart = (bufferStart + copyLeft) % bufferSize;
  }
}

void ConferenceConnection::ReadAndMixAudio(BYTE * data, PINDEX amount, PINDEX channels, unsigned short echoLevel, unsigned sampleRate, unsigned codecChannels)
{ 
  PWaitAndSignal mutex(audioBufferMutex);
  
  if(outgoingSampleRate != sampleRate || outgoingCodecChannels != codecChannels)
  { // sample rate or stereo mode changed
    PTRACE(5,"Mixer\tChanged outgoing sample rate: " << outgoingSampleRate << "->" << sampleRate << ", codec channels: " << outgoingCodecChannels << "->" << codecChannels);
    PCM_BUFFER_SIZE_CALC(sampleRate,codecChannels);
    memset(buffer.GetPointer(),0,bufferSize);
    bufferStart=0; bufferLen=0;
    outgoingSampleRate=sampleRate;
    outgoingCodecChannels=codecChannels;
  }

  if (amount <= 0) { PTRACE(6, "Mixer\tConn " << id << " requested 0 bytes"); return;}

  if(bufferLen < amount)
  {
    if(!hasUnderflow) { PTRACE(6,"Mixer\tConn " << id << " audio buffer underflow: needs " << amount << ", has " << bufferLen << " bytes - silenced"); }
    hasUnderflow = TRUE;
    return;
  }

  if(hasUnderflow)
  {
    if (bufferSize/bufferLen > 2) // waiting at least one half of buffer to continue mixing
    {
      PTRACE(6,"Mixer\tConn " << id << " accumulating audio buffer after underflow, " << (bufferLen*100/bufferSize) << "%, still silencing");
      return;
    }
    else hasUnderflow = FALSE;
  }

  // only mix up to the amount of data remaining
  PINDEX copyLeft = PMIN(amount, bufferLen);

  // if buffer is wrapping, get first part
  if ((bufferStart + copyLeft) > bufferSize) {
    PINDEX toCopy = bufferSize - bufferStart;

    Mix(data, (BYTE *)((unsigned long)buffer.GetPointer() + bufferStart), toCopy, channels, echoLevel, sampleRate);

    data        += toCopy;
    bufferLen   -= toCopy;
    copyLeft    -= toCopy;
    bufferStart = 0;
  }

  // get the remainder of the buffer
  if (copyLeft > 0) {

    Mix(data, (BYTE *)((unsigned long)buffer.GetPointer() + bufferStart), copyLeft, channels, echoLevel, sampleRate);

    bufferLen -= copyLeft;
    bufferStart = (bufferStart + copyLeft) % bufferSize;
  }
}

void ConferenceConnection::Mix(BYTE * dst, const BYTE * src, PINDEX count, PINDEX /*channels*/, unsigned short echoLevel, unsigned sampleRate)
{
  PINDEX i = count >> 1;
  do
  {
    short dstVal = *(short *)dst;
    short srcVal = *(short *)src;
    int newVal = dstVal;                                     // 16-bit to 32-bit, signed
    newVal += srcVal;                                        // mix
    if     (newVal >  0x7fff) *(short *)dst =  0x7fff;       // 16-bit limiter "+"
    else if(newVal < -0x8000) *(short *)dst = -0x8000;       // 16-bit limiter "-"
    else                      *(short *)dst = (short)newVal;
    src += 2;
    dst += 2;
  } while (--i);
}

///////////////////////////////////////////////////////////////

MCULock::MCULock()
{
  closing = FALSE;
  count = 0;
}

BOOL MCULock::Wait(BOOL hard)
{
  mutex.Wait();
  if (hard)
    return TRUE;

  BOOL ret = TRUE;
  if (!closing)
    count++;
  else
    ret = FALSE;

  mutex.Signal();
  return ret;
}

void MCULock::Signal(BOOL hard)
{
  if (hard) {
    mutex.Signal();
    return;
  }

  mutex.Wait();
  if (count > 0)
    count--;
  if (closing)
    closeSync.Signal();
  mutex.Signal();
}

void MCULock::WaitForClose()
{
  mutex.Wait();
  closing = TRUE;
  BOOL wait = count > 0;
  mutex.Signal();
  while (wait) {
    closeSync.Wait();
    mutex.Wait();
    wait = count > 0;
    mutex.Signal();
  }
}
