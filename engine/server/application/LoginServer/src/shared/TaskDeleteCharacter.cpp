// ======================================================================
//
// TaskDeleteCharacter.cpp
// copyright (c) 2001 Sony Online Entertainment
//
// ======================================================================

#include "FirstLoginServer.h"
#include "TaskDeleteCharacter.h"

#include "ClientConnection.h"
#include "LoginServer.h"
#include "DatabaseConnection.h"
#include "sharedDatabaseInterface/DbSession.h"

// ======================================================================

TaskDeleteCharacter::TaskDeleteCharacter(uint32 clusterId, const NetworkId &characterId, StationId stationId) :
		TaskRequest(),
		m_clusterId(clusterId),
		m_characterId(characterId),
		m_stationId(stationId),
		m_succeeded(false)
{
}

// ----------------------------------------------------------------------

bool TaskDeleteCharacter::process(DB::Session *session)
{
	DeleteCharacterQuery qry;
	qry.cluster_id=m_clusterId; //lint !e713 // loss ofp recision unsigned long to long
	qry.character_id=m_characterId;
	qry.station_id=m_stationId; //lint !e713 // loss ofp recision unsigned long to long
	
	bool rval = session->exec(&qry);
	m_succeeded = rval;

	qry.done();
	return rval;
}

// ----------------------------------------------------------------------

void TaskDeleteCharacter::onComplete()
{
	if (!m_succeeded)
	{
		WARNING(true, ("TaskDeleteCharacter: login.delete_character failed for character %s stationId %u cluster %u", m_characterId.getValueString().c_str(), m_stationId, m_clusterId));
	}

	ClientConnection* target = LoginServer::getInstance().getValidatedClient(m_stationId);
	if (target)
	{
		if (m_succeeded)
			target->onCharacterDeletedFromLoginDatabase(m_characterId);
		else
			target->onCharacterDeleteFailed(m_characterId);
	}
}

// ======================================================================

TaskDeleteCharacter::DeleteCharacterQuery::DeleteCharacterQuery() :
		Query(),
		cluster_id(),
		character_id(),
		station_id()
{
}

// ----------------------------------------------------------------------

void TaskDeleteCharacter::DeleteCharacterQuery::getSQL(std::string &sql)
{
	sql = std::string("begin ")+DatabaseConnection::getInstance().getSchemaQualifier()+"login.delete_character(:cluster_id, :character_id, :station_id); end;";
	// DEBUG_REPORT_LOG(true, ("TaskDeleteCharacter SQL: %s\n", sql.c_str()));

}

// ----------------------------------------------------------------------

bool TaskDeleteCharacter::DeleteCharacterQuery::bindParameters()
{
	if (!bindParameter(cluster_id)) return false;
	if (!bindParameter(character_id)) return false;
	if (!bindParameter(station_id)) return false;
	return true;
}

// ----------------------------------------------------------------------

bool TaskDeleteCharacter::DeleteCharacterQuery::bindColumns()
{
	return true;
}

// ----------------------------------------------------------------------

DB::Query::QueryMode TaskDeleteCharacter::DeleteCharacterQuery::getExecutionMode() const
{
	return MODE_PROCEXEC;
}

// ======================================================================
