//////////////////////////////////////////////////
// Blabber [JabberSpeak.cpp]
//////////////////////////////////////////////////

#include <gloox/jid.h>
#include <gloox/dataformitem.h>
#include <gloox/disco.h>
#include <gloox/registration.h>
#include <gloox/rostermanager.h>

#include <cstdio>
#include <Roster.h>
#include <unistd.h>
#include <sys/utsname.h>

#include "BlabberApp.h"
#include "AgentList.h"
#include "GenericFunctions.h"
#include "JabberSpeak.h"
#include "JRoster.h"
#include "../support/LogHandler.h"
#include "../ui/ModalAlertFactory.h"
#include "MessageRepeater.h"
#include "Messages.h"
#include "TalkManager.h"
#include "UserID.h"
#include "XMLEntity.h"

#include <stdlib.h>

#include <Path.h>
#include <FindDirectory.h>
#include <AppFileInfo.h>
#include <File.h>

JabberSpeak *JabberSpeak::_instance = NULL;

//////////////////////////////////////////////////
// CREATORS
//////////////////////////////////////////////////

// FIXME use the GlooxHandler instead for all XMPP things

JabberSpeak *JabberSpeak::Instance() {
	if (_instance == NULL) {
		_instance = new JabberSpeak();
	}
	
	return _instance;
}

JabberSpeak::JabberSpeak()
	: XMLReader()
	, fClient(NULL)
{
	// grab a handle to the settings now for convenience later
	_blabber_settings = BlabberSettings::Instance();
}

JabberSpeak::~JabberSpeak() {
	_instance = NULL;
}

void JabberSpeak::Reset() {
	if (!_reconnecting) {
		BlabberMainWindow::Instance()->Lock();
		BlabberMainWindow::Instance()->ShowLogin();
		BlabberMainWindow::Instance()->Unlock();
	}
	
	if (!_reconnecting) {
		BookmarkManager::Instance().Disconnect();
		TalkManager::Instance()->Reset();
	}
	
	JRoster::Instance()->Lock();
	JRoster::Instance()->RemoveAllUsers();
	JRoster::Instance()->Unlock();

	JRoster::Instance()->RefreshRoster();
	
	// reset agent list
	AgentList::Instance()->RemoveAllAgents();
}

void JabberSpeak::JabberSpeakReset() {
	_curr_realname           = "";
	_curr_login              = "";
	_password                = "";
	_am_logged_in            = false;
	_reconnecting            = false;
	_got_some_agent_info     = false;

	_iq_map.clear();
}

//////////////////////////////////////////////////
// STANDARD METHODS
//////////////////////////////////////////////////

char **JabberSpeak::CreateAttributeMemory(int num_items) {
	char **atts;
	
	atts = (char **)malloc((num_items + 2) * sizeof(char *));
	for (int i=0; i<num_items; ++i)
		atts[i] = (char *)malloc(96 * sizeof(char));
	
	atts[num_items] = NULL;
	atts[num_items+1] = NULL;
	
	return atts;
}

void JabberSpeak::DestroyAttributeMemory(char **atts, int num_items) {
	for (int i=0; i<(num_items + 2); ++i) {
		free(atts[i]);
	}
	
	free(atts);
}

const string JabberSpeak::CurrentRealName() const {
	return _curr_realname;
}

const string JabberSpeak::CurrentLogin() const {
	return _curr_login;
}

//////////////////////////////////////////////////
// INCOMING COMMUNICATION
//////////////////////////////////////////////////

void JabberSpeak::OnTag(XMLEntity *entity) {
	char buffer[4096]; // general buffer space
	string iq_id;      // used for IQ tags

	static int seen_streams = 0;

	// handle disconnection
	if (entity->IsCompleted() && !strcasecmp(entity->Name(), "stream:stream")) {
		++seen_streams;
		
		if (seen_streams % 2 == 1) {
			Reset();

			MessageRepeater::Instance()->PostMessage(JAB_DISCONNECTED);
		}

		return;
	}

	// handle IQs (always closing tags)
	if (entity->IsCompleted() && !strcasecmp(entity->Name(), "iq")) {
		if (entity->Attribute("id")) {
			iq_id = entity->Attribute("id");
		}
		
		if (!strcasecmp(entity->Attribute("type"), "error")) {
			// get the intent of the IQ message
			if (_iq_map.count(iq_id) > 0) {
				// process based on the intent
				iq_intent intent = _iq_map[iq_id];

				// for errors on login				
				if (intent == LOGIN) {
					sprintf(buffer, "Your login attempt failed due to the following reason:\n\n%s", entity->Child("error")->Data());
					ModalAlertFactory::Alert(buffer, "I'll try again", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT); 

					Reset();
				}

				// for errors on registration
				if (intent == REGISTER) {
					if (entity->Attribute("from") && AgentList::Instance()->GetAgentByID(entity->Attribute("from"))) {
						const char *agent_name = AgentList::Instance()->GetAgentByID(entity->Attribute("from"))->Name().c_str();
						sprintf(buffer, "You were refused registration information from the %s for the following reason:\n\n%s", agent_name, entity->Child("error")->Data());
					} else {
						sprintf(buffer, "You were refused registration information from an unidentifying Jabber service for the following reason:\n\n%s", entity->Child("error")->Data());
					}

					ModalAlertFactory::Alert(buffer, "Oh, well.", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT); 
				}

				// for errors on registration
				if (intent == SEND_REGISTER) {
					if (entity->Attribute("from") && AgentList::Instance()->GetAgentByID(entity->Attribute("from"))) {
						const char *agent_name = AgentList::Instance()->GetAgentByID(entity->Attribute("from"))->Name().c_str();
						sprintf(buffer, "Your registration attempt was refused by the %s for the following reason:\n\n%s", agent_name, entity->Child("error")->Data());
					} else {
						sprintf(buffer, "Your registration attempt was refused by an unidentifying Jabber service for the following reason:\n\n%s", entity->Child("error")->Data());
					}

					ModalAlertFactory::Alert(buffer, "Oh, well.", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT); 
				}

				// for errors on unregistration
				if (intent == UNREGISTER) {
					if (entity->Attribute("from") && AgentList::Instance()->GetAgentByID(entity->Attribute("from"))) {
						const char *agent_name = AgentList::Instance()->GetAgentByID(entity->Attribute("from"))->Name().c_str();
						sprintf(buffer, "You were refused unregistration information from the %s for the following reason:\n\n%s", agent_name, entity->Child("error")->Data());
					} else {
						sprintf(buffer, "You were refused unregistration information from an unidentifying Jabber service for the following reason:\n\n%s", entity->Child("error")->Data());
					}

					ModalAlertFactory::Alert(buffer, "Oh, well.", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT); 
				}
			}
		}

		// handle RESULT only (success)
		if (!strcasecmp(entity->Attribute("type"), "result")) {
			// get the intent of the IQ message
			if (_iq_map.count(iq_id) > 0) {
				// process based on the intent
				iq_intent intent = _iq_map[iq_id];

				// for the agents list
				if (intent == AGENTS) {
					_ParseAgentList(entity);
				}

				// for completed registration
				if (intent == SEND_REGISTER) {
					if (entity->Attribute("from") && AgentList::Instance()->GetAgentByID(entity->Attribute("from"))) {
						const char *agent_name = AgentList::Instance()->GetAgentByID(entity->Attribute("from"))->Name().c_str();
						sprintf(buffer, "Your registration attempt with the %s has been accepted.", agent_name);

						AgentList::Instance()->GetAgentByID(entity->Attribute("from"))->SetRegisteredFlag(true);			
					} else {
						sprintf(buffer, "Your registration attempt with an unidentifying Jabber service has been accepted.");
					}

					ModalAlertFactory::Alert(buffer, "Yeah!", NULL, NULL, B_WIDTH_AS_USUAL, B_STOP_ALERT); 
				}
			}
		}
	}
}

void JabberSpeak::_ParseAgentList(XMLEntity *iq_agent_entity) {
	XMLEntity *entity = iq_agent_entity;
	
	// go one level deep to query
	if (entity->Child("query")) {
		entity = entity->Child("query");
	} else {
		return;
	}
	
	// iterate through child 'item' tags
	for (int i=0; i<entity->CountChildren(); ++i) {
		// handle the item child
		if (!strcasecmp(entity->Child(i)->Name(), "agent")) {
			// create a new agent
			Agent *agent = new Agent();

			// add data about the agent
			if (entity->Child(i)->Attribute("jid")) {
				agent->SetJID(entity->Child(i)->Attribute("jid"));
			}

			if (entity->Child(i)->Child("name")) {
				agent->SetName(entity->Child(i)->Child("name")->Data());
			}

			if (entity->Child(i)->Child("description")) {
				agent->SetDescription(entity->Child(i)->Child("description")->Data());
			}

			if (entity->Child(i)->Child("service")) {
				agent->SetService(entity->Child(i)->Child("service")->Data());
			}

			agent->SetRegisterableFlag(entity->Child(i)->Child("register"));

			if (entity->Child(i)->Child("transport")) {
				agent->SetTransport(entity->Child(i)->Child("transport")->Data());
			}

			agent->SetSearchableFlag(entity->Child(i)->Child("search"));

			// add to the AgentList BUGBUG need a return value
			AgentList::Instance()->AddAgent(agent);
		}
	}

	_got_some_agent_info = true;
	_am_logged_in = true;
}

void JabberSpeak::_AcceptPresence(string username) {
	gloox::Subscription subscription(gloox::Subscription::Subscribed, gloox::JID(username));
	fClient->send(subscription);
}

void JabberSpeak::_RejectPresence(string username) {
	gloox::Subscription subscription(gloox::Subscription::Unsubscribed, gloox::JID(username));
	fClient->send(subscription);
}

//////////////////////////////////////////////////
// OUTGOING COMMUNICATION
//////////////////////////////////////////////////

void JabberSpeak::SendConnect(string username, string password, string realname, bool suppress_auto_connect) {

	// if there's another application instance running, suppress auto-login
	BList *teams = new BList;
	
	// query for this app signature
	be_roster->GetAppList("application/jabber", teams);
	
	if ((username.size() == 0 || password.size() == 0) && teams->CountItems() > 1) {
		suppress_auto_connect = true;
	}
	
	// if we don't have all the data, query for it
	if (username.size() == 0 || password.size() == 0 || gloox::JID(username).server().size() == 0) {
		// check auto-login 
		if (suppress_auto_connect == false && _blabber_settings->Tag("auto-login")) {
			// last login data should be used (make sure it's there though)
			realname = (_blabber_settings->Data("last-realname")) ? _blabber_settings->Data("last-realname") : "";
			username = (_blabber_settings->Data("last-login")) ? _blabber_settings->Data("last-login") : "";
			password = (_blabber_settings->Data("last-password")) ? _blabber_settings->Data("last-password") : "";
		}

		// if we still don't have all the data, query for it
		if (username.size() == 0 || password.size() == 0 || gloox::JID(username).server().size() == 0) {
			BlabberMainWindow::Instance()->Lock();
			BlabberMainWindow::Instance()->ShowLogin();
			BlabberMainWindow::Instance()->Unlock();
			return;
		}
	}

	// save username/password
	_curr_realname = realname;
	_curr_login    = username;
	_password      = password;

	// PLACEHOLDER
	// spawn listener thread (communication from remote machine)
	resume_thread(_connection_thread_id = spawn_thread(JabberSpeak::_SpawnConnectionThread, "connection_listener", B_LOW_PRIORITY, this));
}


int32 JabberSpeak::_SpawnConnectionThread(void *obj) {
	((JabberSpeak *)obj)->_ConnectionThread();

	// Don't care about the return value
	return 1;
}


string					
JabberSpeak::GetRealServer()
{
	return gloox::JID(_curr_login).server();
}

int	
JabberSpeak::GetRealPort()
{
	return 5222; //default jabber port.
}	


void JabberSpeak::_ConnectionThread() {
	gloox::JID jid(_curr_login);
	fClient = new gloox::Client(jid, _password);

	fClient->rosterManager()->registerRosterListener(JRoster::Instance());
	fClient->logInstance().registerLogHandler(gloox::LogLevelDebug,
		gloox::LogAreaXmlOutgoing, new LogHandler);
	fClient->registerConnectionListener(this);
	fClient->registerMessageHandler(TalkManager::Instance());
	_ProcessVersionRequest();

	fClient->connect();
}


void JabberSpeak::SendDisconnect() {
	XMLEntity *end_stream;
	
	_am_logged_in = false;

	end_stream = new XMLEntity("stream:stream", NULL);
	
	char *str = end_stream->EndToString();
	free(str);

	delete end_stream;
}

void JabberSpeak::SendSubscriptionRequest(string username) {
	gloox::Subscription subscription(gloox::Subscription::Subscribe,
		gloox::JID(username));
	fClient->send(subscription);

	// log command
	_iq_map[subscription.id()] = LOGIN;
}

void JabberSpeak::SendUnsubscriptionRequest(string username) {
	gloox::Subscription subscription(gloox::Subscription::Unsubscribe,
		gloox::JID(username));
	fClient->send(subscription);
}

void JabberSpeak::SetFriendlyName(const gloox::JID& who, BString friendlyName)
{
	gloox::RosterItem* item = fClient->rosterManager()->getRosterItem(who);
	if (item) {
		item->setName(friendlyName.String());
		fClient->rosterManager()->synchronize();
	} else {
		const gloox::ConferenceListItem* bookmark
			= BookmarkManager::Instance().GetBookmark(who.full().c_str());
		if (bookmark) {
			BookmarkManager::Instance().SetBookmark(who.full().c_str(),
				bookmark->nick.c_str(), friendlyName, bookmark->autojoin);
		} else {
			debugger("User not found.");
		}
	}
}

void JabberSpeak::RemoveFromRoster(const UserID *removed_user) {
	fClient->rosterManager()->remove(gloox::JID(removed_user->Handle()));
}


void JabberSpeak::SendMessage(const gloox::Message::MessageType type,
	const UserID *user, string body, string thread_id)
{
	gloox::Message message(type,
		gloox::JID(user->JabberCompleteHandle()), body, gloox::EmptyString,
		thread_id);
	fClient->send(message);
}


void JabberSpeak::SendMessage(const gloox::Message::MessageType type,
	string group_room, string body) {
	gloox::Message message(type, gloox::JID(group_room), body);
	fClient->send(message);
}

void JabberSpeak::SendPresence(gloox::Presence::PresenceType type, string status) {
	gloox::Presence presence(type, gloox::JID(), status);
	fClient->send(presence);
}

void JabberSpeak::SendLastPresence() {
	bool last_used_custom   = BlabberSettings::Instance()->Tag("last-used-custom-status");
	gloox::Presence::PresenceType last_status = (gloox::Presence::PresenceType)atoi(
		BlabberSettings::Instance()->Data("last-custom-exact-status"));
	const char *last_custom_status = BlabberSettings::Instance()->Data("last-custom-more-exact-status");

	if (last_used_custom) {
		SendPresence(last_status, last_custom_status);
		BlabberMainWindow::Instance()->SetCustomStatus(last_custom_status);
	} else {
		if (last_status == gloox::Presence::Chat) {
			BlabberMainWindow::Instance()->PostMessage(BLAB_AVAILABLE_FOR_CHAT);
		} else if (last_status == gloox::Presence::Away) {
			BlabberMainWindow::Instance()->PostMessage(BLAB_AWAY_TEMPORARILY);
		} else if (last_status == gloox::Presence::XA) {
			BlabberMainWindow::Instance()->PostMessage(BLAB_AWAY_EXTENDED);
		} else if (last_status == gloox::Presence::DND) {
			BlabberMainWindow::Instance()->PostMessage(BLAB_DO_NOT_DISTURB);
		} else {
			BlabberMainWindow::Instance()->PostMessage(BLAB_AVAILABLE_FOR_CHAT);
		}
	}
}

void JabberSpeak::SendGroupUnvitation(string _group_room, string _group_username) {
	// assemble group ID
	string group_presence = _group_room + "/" + _group_username;	
	
	// Send presence Stanza
	gloox::Presence presence(gloox::Presence::Unavailable, gloox::JID(group_presence));
	fClient->send(presence);

	// Disable autologin in bookmarks and store username
	const gloox::ConferenceListItem* item
		= BookmarkManager::Instance().GetBookmark(_group_room.c_str());
	if (item) {
		BookmarkManager::Instance().SetBookmark(_group_room.c_str(),
			_group_username.c_str(), item->name.c_str(), false);
	}
}

void JabberSpeak::RegisterWithAgent(string agent) {
	XMLEntity          *entity;
	char **atts       = CreateAttributeMemory(6);
	char **atts_query = CreateAttributeMemory(2);

	// assemble attributes;
	strcpy(atts[0], "type");
	strcpy(atts[1], "get");

	strcpy(atts[2], "id");
	strcpy(atts[3], GenericFunctions::GenerateUniqueID().c_str());

	strcpy(atts[4], "to");
	strcpy(atts[5], AgentList::Instance()->GetAgentByService(agent)->JID().c_str());

	strcpy(atts_query[0], "xmlns");
	strcpy(atts_query[1], "jabber:iq:register");

	// send XML command
	entity = new XMLEntity("iq", (const char **)atts);
	entity->AddChild("query", (const char **)atts_query, NULL);

	// log command
	_iq_map[atts[3]] = REGISTER;
	
	char *str = entity->ToString();
	free(str);

	DestroyAttributeMemory(atts, 6);
	DestroyAttributeMemory(atts_query, 2);
	
	delete entity;
}

void JabberSpeak::UnregisterWithAgent(string agent) {
	// Find registration in roster
	JRoster::Instance()->Lock();

	UserID *user = NULL;
	if (AgentList::Instance()->GetAgentByService(agent)) {
		user = JRoster::Instance()->FindUser(JRoster::TRANSPORT_ID, AgentList::Instance()->GetAgentByService(agent)->JID());
	}
	
	// call RemoveFromRoster()
	if (user) {
		RemoveFromRoster(user);
	}

	JRoster::Instance()->Unlock();
}

void JabberSpeak::_ProcessVersionRequest(void) {
	string strVersion("Haiku");
	BPath path;
	if (find_directory(B_BEOS_LIB_DIRECTORY, &path) == B_OK) {
		path.Append("libbe.so");

		BAppFileInfo appFileInfo;
		version_info versionInfo;
		BFile file;
		if (file.SetTo(path.Path(), B_READ_ONLY) == B_OK
			&& appFileInfo.SetTo(&file) == B_OK
			&& appFileInfo.GetVersionInfo(&versionInfo, 
				B_APP_VERSION_KIND) == B_OK
			&& versionInfo.short_info[0] != '\0')
				strVersion = versionInfo.short_info;

		if (strVersion == "Walter")
			strVersion = "Haiku";

		BString appVersion;
		appVersion << " " << versionInfo.major << "." << versionInfo.middle;
		if (versionInfo.minor > 0)
			appVersion << "." << versionInfo.minor;

		// Add the version variety
		const char* variety = NULL;
		switch (versionInfo.variety) {
			case B_DEVELOPMENT_VERSION:
				variety = "development";
				break;
			case B_ALPHA_VERSION:
				variety = "alpha";
				break;
			case B_BETA_VERSION:
				variety = "beta";
				break;
			case B_GAMMA_VERSION:
				variety = "gamma";
				break;
			case B_GOLDEN_MASTER_VERSION:
				variety = "gold master";
				break;
		}

		if (variety)
			appVersion << "-" << variety;

		strVersion += appVersion;
	}

	fClient->disco()->setVersion("Renga", APP_VERSION, strVersion);
}

void
JabberSpeak::onConnect()
{
	fprintf(stderr, "Logged in!\n");
	MessageRepeater::Instance()->PostMessage(JAB_LOGGED_IN);
	//SendLastPresence();	
	
	BookmarkManager::Instance().Connect();

	_reconnecting = false;
}


void
JabberSpeak::onDisconnect(gloox::ConnectionError e)
{

	BookmarkManager::Instance().Disconnect();
	if (e == gloox::ConnAuthenticationFailed) {
		gloox::AuthenticationError ae = fClient->authError();

		if (ae == gloox::SaslNotAuthorized) {
			// Incorrect login or password, highlight them in main window
			// The code below gets us back to login screen
			SendNotices(kAuthenticationFailed);
		} else {
			fprintf(stderr, "%s(%d) -> %d\n", __PRETTY_FUNCTION__, e, ae);
		}
	} else {
		fprintf(stderr, "%s(%d)\n", __PRETTY_FUNCTION__, e);
	}

	// reset XMLReader
	XMLReader::Reset();

	if (_am_logged_in) {
		// automatic reconnection
		_reconnecting = true;
	
		MessageRepeater::Instance()->PostMessage(JAB_RECONNECTING);

		_got_some_agent_info     = false;
		_am_logged_in            = false;

		// reset networking
		Reset();

		SendConnect();
	}
}

bool
JabberSpeak::onTLSConnect(__attribute__((unused)) const gloox::CertInfo& info)
{
	// TODO verify certificate
	return true;
}

