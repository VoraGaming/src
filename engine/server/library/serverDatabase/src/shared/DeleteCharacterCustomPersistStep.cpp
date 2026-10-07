// ======================================================================
//
// DeleteCharacterCustomPersistStep.cpp
// copyright (c) 2001 Sony Online Entertainment
//
// ======================================================================

#include "serverDatabase/FirstServerDatabase.h"
#include "serverDatabase/DeleteCharacterCustomPersistStep.h"

#include "serverDatabase/ConfigServerDatabase.h"
#include "serverDatabase/DatabaseProcess.h"
#include "sharedDatabaseInterface/DbSession.h"
#include "sharedFoundation/NetworkIdArchive.h"
#include "sharedFoundation/StationId.h"
#include "sharedLog/Log.h"
#include "sharedNetworkMessages/GenericValueTypeMessage.h"

// ======================================================================

DeleteCharacterCustomPersistStep::DeleteCharacterCustomPersistStep(uint32 stationId, const NetworkId &characterId, uint32 loginServerId) :
	m_characterId(characterId),
	m_stationId(stationId),
	m_loginServerId(loginServerId),
	m_resultCode(-1)
{
}

// ----------------------------------------------------------------------

bool DeleteCharacterCustomPersistStep::beforePersist(DB::Session *)
{
	return true;
}

// ----------------------------------------------------------------------

bool DeleteCharacterCustomPersistStep::afterPersist(DB::Session *session)
{
	DeleteCharacterQuery qry(m_stationId, m_characterId);

	if (!(session->exec(&qry)))
	{
		m_resultCode = -1;
		return false;
	}
	qry.done();

	m_resultCode = static_cast<int32>(qry.result.getValue());
	return true;
}

// ----------------------------------------------------------------------

void DeleteCharacterCustomPersistStep::onComplete()
{
	if (m_resultCode == 2)
	{
		GenericValueTypeMessage<NetworkId> msg("ReleaseCharacterNameByIdMessage", m_characterId);
		DatabaseProcess::getInstance().sendToAllGameServers(msg, true);
	}
	else if (m_resultCode == 1)
	{
		LOG("CustomerService", ("Player:WARNING delete of character %s for stationId %u failed: persister.delete_character returned 1 (no players row for this character and account). The character was not deleted.", m_characterId.getValueString().c_str(), m_stationId));
		WARNING(true, ("DeleteCharacterCustomPersistStep: persister.delete_character returned 1 for character %s stationId %u", m_characterId.getValueString().c_str(), m_stationId));
	}

	// report the result to the login server that asked (through Central), so it only removes its row on success
	if (m_loginServerId != 0)
	{
		GenericValueTypeMessage<std::pair<std::pair<uint32, StationId>, std::pair<NetworkId, int32> > > const reply("ServerDeleteCharacterReply", std::make_pair(std::make_pair(m_loginServerId, static_cast<StationId>(m_stationId)), std::make_pair(m_characterId, m_resultCode)));
		DatabaseProcess::getInstance().sendToCentralServer(reply, true);
	}
}

// ======================================================================

DeleteCharacterCustomPersistStep::DeleteCharacterQuery::DeleteCharacterQuery(uint32 stationId, const NetworkId &characterId) :
	station_id(stationId),
	character_id(characterId),
	delete_minutes(ConfigServerDatabase::getCharacterImmediateDeleteMinutes()),
	result()
{
}

// ----------------------------------------------------------------------

void DeleteCharacterCustomPersistStep::DeleteCharacterQuery::getSQL(std::string &sql)
{
	sql = "begin :result := " + DatabaseProcess::getInstance().getSchemaQualifier() + "persister.delete_character (:station_id, :character_id, :delete_minutes); end;";
}

// ----------------------------------------------------------------------

bool DeleteCharacterCustomPersistStep::DeleteCharacterQuery::bindParameters()
{
	if (!bindParameter(result)) return false;
	if (!bindParameter(station_id)) return false;
	if (!bindParameter(character_id)) return false;
	if (!bindParameter(delete_minutes)) return false;
	return true;
}

// ----------------------------------------------------------------------

bool DeleteCharacterCustomPersistStep::DeleteCharacterQuery::bindColumns()
{
	return true;
}

// ----------------------------------------------------------------------

DB::Query::QueryMode DeleteCharacterCustomPersistStep::DeleteCharacterQuery::getExecutionMode() const
{
	return DB::Query::MODE_PROCEXEC;
}

// ======================================================================