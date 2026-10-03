// ======================================================================
//
// CellPermissions.cpp
//
// Copyright 2002 Sony Online Entertainment
//
// Refactored - SWG Source - 2021 (Aconite)
//
// ======================================================================

#include <utility>
#include <cstdlib>
#include <cerrno>
#include <climits>

#include "serverGame/FirstServerGame.h"
#include "serverGame/CellPermissions.h"

#include "UnicodeUtils.h"
#include "serverGame/BuildingObject.h"
#include "serverGame/CellObject.h"
#include "serverGame/Client.h"
#include "serverGame/ContainerInterface.h"
#include "serverGame/CreatureObject.h"
#include "serverGame/GuildInterface.h"
#include "serverGame/CityInterface.h"
#include "serverGame/CityInfo.h"
#include "serverGame/ServerWorld.h"
#include "serverGame/NameManager.h"
#include "sharedGame/PvpData.h"
#include "sharedFoundation/NetworkId.h"
#include "sharedLog/Log.h"
#include "sharedObject/Container.h"
#include "sharedNetworkMessages/UpdateCellPermissionMessage.h"

// ======================================================================

static std::set<BuildingObject const*> s_activeUpdaters;

// ======================================================================
// Non-throwing id parsers. The permission-list ids here are std::hash-derived
// uint32 station ids and faction CRCs, about half of which sit above
// INT32_MAX (e.g. the Imperial faction CRC 3679112276). std::stoi throws
// std::out_of_range on those and the callers do not catch it, so it crashed
// the game server. These parse without throwing: bad text simply yields 0,
// which the callers treat as "no match" (empty name / not on the list).
// ======================================================================
namespace CellPermissionsNamespace
{
	// ----------------------------------------------------------------------
	/**
	 * Parse a whole decimal integer without throwing. Returns true and sets
	 * @p out if the text is a clean integer (no leading/trailing junk, within
	 * int64 range); otherwise returns false and @p out is unchanged.
	 */
	inline bool parseInteger(std::string const &text, int64 &out)
	{
		if (text.empty())
		{
			return false;
		}
		errno = 0;
		char * end = nullptr;
		const long long value = std::strtoll(text.c_str(), &end, 10);
		if (end == text.c_str())		// no digits consumed
		{
			return false;
		}
		if (*end != '\0')			// trailing non-numeric characters
		{
			return false;
		}
		if (errno == ERANGE)			// out of long long range
		{
			return false;
		}
		out = value;
		return true;
	}

	// ----------------------------------------------------------------------
	/**
	 * Parse a 32-bit signed id (guild/city). Returns 0 if the text is not a
	 * valid int32, so callers treat bad text as "no match".
	 */
	inline int parseInt32(std::string const &text)
	{
		int64 value = 0;
		if (!parseInteger(text, value))
		{
			return 0;
		}
		if (value < INT32_MIN || value > INT32_MAX)
		{
			return 0;
		}
		return static_cast<int>(value);
	}

	// ----------------------------------------------------------------------
	/**
	 * Parse a 32-bit id that may be stored as a signed or unsigned 32-bit
	 * value (station ids and faction CRCs are uint32 and can exceed INT32_MAX).
	 * The Java scripts write ids in signed form, so a value like the Imperial
	 * faction CRC 3679112276 can appear as -615855020: accept both spellings
	 * and wrap a signed value into its unsigned 32-bit form (the original
	 * static_cast<uint32>(std::stoi(...)) behaved the same way). Returns 0 if
	 * the text is outside the signed-32 / unsigned-32 range, so callers treat
	 * bad text as "no match".
	 */
	inline uint32 parseBits32(std::string const &text)
	{
		int64 value = 0;
		if (!parseInteger(text, value))
		{
			return 0;
		}
		if (value < INT32_MIN || value > 0xFFFFFFFFll)
		{
			return 0;
		}
		return static_cast<uint32>(value);
	}
}

// ======================================================================

/**
 * Cell permissions are stored in the PROPERTY_LIST db table. The OBJECT_ID column represents
 * the Network ID of the cell object, the LIST_ID will either be a 3 for the allow list,
 * or a 4 for the banned list of a cell, and the VALUE property will be a suffix indicator
 * of the data type followed by the value (there is a PROPERTY_LIST entry for *each* entry on
 * the respective allow/ban list)
 *
 * Note that allow/banned lists are used in both player structure and NPC-content contexts,
 * but the public/private flag supersedes, in which case if Public -> anyone but banned list,
 * if Private -> no one but allow list
 *
 * Items are stored in the DB as:
 *
 * c:id which is for character networkIds
 * n:id which is for non-player networkIds
 * G:id which is for Guild IDs
 * P:id which is for City IDs
 * A:id which is for Account Station IDs
 * F:id which is for crc faction name
 */
namespace Archive
{

	// ----------------------------------------------------------------------
	/**
	 * Read an Archive Iterator from the Database
	 */
	void get(ReadIterator &source, CellPermissions::PermissionObject &target)
	{
		std::string permissionString;
		get(source, permissionString);
        CellPermissions::PermissionObject::PermissionFormat format;

		// Make sure there is a prefix attached to the entry and remove it
        const std::string prefix(permissionString, 0, 2);
        if(prefix == Prefix::CHARACTER)
        {
            format = CellPermissions::PermissionObject::PF_CHARACTER_NAME;
        }
        else if (prefix == Prefix::GUILD)
        {
            format = CellPermissions::PermissionObject::PF_GUILD_NAME;
        }
        else if (prefix == Prefix::NUMERIC)
        {
            format = CellPermissions::PermissionObject::PF_NUMERIC_VALUE;
        }
        else if (prefix == Prefix::CITY)
        {
            format = CellPermissions::PermissionObject::PF_CITY_NAME;
        }
        else if (prefix == Prefix::ACCOUNT)
        {
            format = CellPermissions::PermissionObject::PF_ACCOUNT_NAME;
        }
        else if (prefix == Prefix::FACTION)
        {
            format = CellPermissions::PermissionObject::PF_FACTION_NAME;
        }
        else
        {
            WARNING_STRICT_FATAL(true, ("CellPermissions::Archive::get() got a source value without a prefix or with a prefix we don't know (%s)."
                "This is VERY game breaking. Please notify Development ASAP.", prefix.c_str()));
            return;
        }
        // remove the prefix
        permissionString.erase(0, 2);
        // pass to buildings/cells who do the rest
		target = CellPermissions::PermissionObject( format, permissionString );
	}

	// ----------------------------------------------------------------------

    /**
	 * Put a passed value into an Archive Iterator String for the Database
	 */
	void put( ByteStream &target, CellPermissions::PermissionObject const &source )
	{
		std::string permissionString = source.m_permissionString;

		// Append appropriate prefix for data type
		switch (source.m_originalPermissionFormat)
		{
			case CellPermissions::PermissionObject::PF_CHARACTER_NAME:
				permissionString = permissionString.insert(0, Prefix::CHARACTER);
				break;
			case CellPermissions::PermissionObject::PF_GUILD_NAME:
				permissionString = permissionString.insert(0, Prefix::GUILD);
				break;
		    case CellPermissions::PermissionObject::PF_NUMERIC_VALUE:
		        permissionString = permissionString.insert(0, Prefix::NUMERIC);
		        break;
		    case CellPermissions::PermissionObject::PF_FACTION_NAME:
		        permissionString = permissionString.insert(0, Prefix::FACTION);
		        break;
		    case CellPermissions::PermissionObject::PF_ACCOUNT_NAME:
		        permissionString = permissionString.insert(0, Prefix::ACCOUNT);
		        break;
		    case CellPermissions::PermissionObject::PF_CITY_NAME:
		        permissionString = permissionString.insert(0, Prefix::CITY);
                break;
			default:
                WARNING_STRICT_FATAL(true, ("CellPermissions::Archive::put() got a source value without a prefix or with a prefix we don't know (%s)."
                                            "This is VERY game breaking. Please notify Development ASAP.", source.m_originalPermissionFormat));
				break;
		}
		put(target, permissionString);
	}

}; // namespace Archive

// ======================================================================

CellPermissions::PermissionObject::PermissionObject() :
	m_originalPermissionFormat( PF_UNCONVERTED ),
	m_permissionString()
{
}

// ----------------------------------------------------------------------

/**
 * When we call either CellObject or BuildingObject :: addAllowed()/addBanned(),
 * we use this constructor for a PermissionObject we can manipulate.
 *
 * @param The permission entry for this object as provided by the PLAYER in
 * the add/remove functionality OR as provided by the SYSTEM (which should also
 * be in a consistent format) like "Guild:SWG" or "APlayerNameHere". EXCLUDING**
 * the Account: and Faction: values, which have been advance-handled/validated
 * in scripting/CommandFuncs due to accessibility of neighbor classes.
 *
 * This should strip prefixes from values so we can rely on
 * m_originalPermissionFormat for the Data Type and
 * m_permissionString as the raw value absent the prefix (if any)
 *
 * Note: This is a programmatic object only, not a type of game object.
 */
CellPermissions::PermissionObject::PermissionObject(const std::string& name) :
	m_originalPermissionFormat(PF_UNKNOWN_STRING),
	m_permissionString(Unicode::getTrim(name))
{
	// If the string starts with "Guild:" we're processing a Guild Name
    if (name.rfind("guild:", 0) == 0)
	{
		// Find Guild ID from Guild Name
		const int guildId = GuildInterface::findGuild(name.substr(6, name.length()));
		if (guildId != 0)
		{
			m_originalPermissionFormat = PF_GUILD_NAME;
			m_permissionString         = std::to_string(guildId);
		}
	}
	// If the string starts with "City:" we're processing a City Name
    else if (name.rfind("city:", 0) == 0)
    {
	    const int cityId = CityInterface::findCityByName(name.substr(5, name.length()));
	    if (cityId != 0)
        {
	        m_originalPermissionFormat = PF_CITY_NAME;
	        m_permissionString = std::to_string(cityId);
        }
	    return;
    }
	// If the string starts with "Account:" we're processing an Account Name
	// which MUST be a stationId NOT a username
    else if (name.rfind("account:", 0) == 0)
    {
        // There's really no way to verify this from here, so
        // we're relying on it being handled elsewhere correctly
        m_originalPermissionFormat = PF_ACCOUNT_NAME;
        m_permissionString = name.substr(8, name.length());
    }
    // If the string starts with "Faction:" it should be the faction hash
    else if (name.rfind("faction:", 0) == 0)
    {
        const uint32 hash = CellPermissionsNamespace::parseBits32(name.substr(8, name.length()));
        if(PvpData::isImperialFactionId(hash) || PvpData::isRebelFactionId(hash))
        {
            m_originalPermissionFormat = PF_FACTION_NAME;
            m_permissionString = name.substr(8, name.length());
        }
    }
	// Otherwise we're processing a NetworkId
    else
	{
	    // if we have only digits, this is a raw networkId
	    if(name.find_first_not_of("0123456789") == std::string::npos)
        {
	        m_originalPermissionFormat = PF_NUMERIC_VALUE;
	        m_permissionString = name;
        }
	    else // otherwise it's a character's network ID
        {
            const NetworkId playerNetworkId = NameManager::getInstance().getPlayerId( NameManager::normalizeName(name));
            if (playerNetworkId != NetworkId::cms_invalid)
            {
                m_originalPermissionFormat = PF_CHARACTER_NAME;
                m_permissionString         = playerNetworkId.getValueString();
            }
        }
	}
}

// ----------------------------------------------------------------------

CellPermissions::PermissionObject::PermissionObject( PermissionFormat format, std::string  name ) :
	m_originalPermissionFormat( format ),
	m_permissionString(std::move( name ))
{
}

// ----------------------------------------------------------------------

bool CellPermissions::PermissionObject::operator<(PermissionObject const &rhs) const
{
	return (m_permissionString < rhs.m_permissionString);
}

// ----------------------------------------------------------------------

/**
 * @return the "name" value (m_permissionString) of this Permission Object, which
 * is what should be rendered to the player as the value on a permission list. This
 * is only accessible after the constructor has determined the data type and stripped
 * the property list value prefix.
 *
 * **NOTE** getName() has validation handling in Cell Object or Building Object
 * so if the value is no longer valid (e.g. deleted guild, orphaned creature object,
 * etc.) then return nullptr so the onLoadedFromDatabase() processes will know
 * this value is bad and needs to be purged.
 */
std::string CellPermissions::PermissionObject::getName() const
{
	std::string nameString;

	switch (m_originalPermissionFormat)
    {
        case PF_CHARACTER_NAME:
        case PF_NUMERIC_VALUE:
        {
            const NetworkId networkId = NetworkId(m_permissionString);
            if(networkId != NetworkId::cms_invalid)
            {
                // player name
                if(m_originalPermissionFormat == PF_CHARACTER_NAME)
                {
                    const std::string fullName = NameManager::getInstance().getPlayerFullName(networkId);
                    nameString = fullName.substr(0, fullName.find(' '));
                }
                // numeric id
                else
                {
                    nameString = m_permissionString;
                }
            }
            break;
        }
        case PF_GUILD_NAME:
        {
            const int guildId = CellPermissionsNamespace::parseInt32(m_permissionString);
            if(GuildInterface::guildExists(guildId))
            {
                nameString = "guild:" + GuildInterface::getGuildAbbrev(guildId);
            }
            break;
        }
        case PF_CITY_NAME:
        {
            const int cityId = CellPermissionsNamespace::parseInt32(m_permissionString);
            if(CityInterface::cityExists(cityId))
            {
                nameString = "city:" + CityInterface::getCityInfo(cityId).getCityName();
            }
            break;
        }
        case PF_ACCOUNT_NAME:
        {
            // There isn't a good way to verify this here, so we're
            // returning the Account followed by the StationID. Because
            // right now, a player can only add their OWN account, we will
            // just insert their account name from the stationID at the
            // script level when rendering a permission list SUI window.
            // And because a deletion scenario for an entire account is
            // an ultra-rare circumstance (and because we can't really verify
            // it in an existing way anyways, we'll assume it's valid).
            nameString = "account:" + m_permissionString;
            break;
        }
        case PF_FACTION_NAME:
        {
            const uint32 hash = CellPermissionsNamespace::parseBits32(m_permissionString);
            if(PvpData::isImperialFactionId(hash))
            {
                nameString = "faction:Imperial";
                break;
            }
            if(PvpData::isRebelFactionId(hash))
            {
                nameString = "faction:Rebel";
                break;
            }
            break;
        }
        default:
        {
            break;
        }
    }
    return nameString;
}

// ----------------------------------------------------------------------

bool CellPermissions::PermissionObject::hasBeenConverted() const
{
	return (m_originalPermissionFormat != PF_UNCONVERTED);
}

// ======================================================================

CellPermissions::UpdateObserver::UpdateObserver(CellObject *cell, Archive::AutoDeltaObserverOp) :
	m_profilerBlock("CellPermissions::UpdateObserver - cell"),
	m_building(nullptr),
	m_permissions()
{
	if (cell)
	{
		BuildingObject *building = cell->getOwnerBuilding();
		if (building)
			init(building);
	}
}

// ----------------------------------------------------------------------

CellPermissions::UpdateObserver::UpdateObserver(TangibleObject *tangible, Archive::AutoDeltaObserverOp) :
	m_profilerBlock("CellPermissions::UpdateObserver - building"),
	m_building(nullptr),
	m_permissions()
{
	if (tangible)
	{
		BuildingObject * const building = tangible->asBuildingObject();
		if (building)
			init(building);
	}
}

// ----------------------------------------------------------------------

CellPermissions::UpdateObserver::~UpdateObserver()
{
	if (m_building)
	{
		s_activeUpdaters.erase(m_building);
		// send permission updates for any changes to any cells of this building,
		// and collect list of creatures we need to expel due to these changes.
		unsigned int permPos = 0;
		std::vector<CreatureObject*> expel;
		handleCellPermissionsUpdateIfNeeded(*m_building, permPos, expel);
		// expel any creatures from the building that no longer belong.
		for (auto i : expel)
			m_building->expelObject(*i);
	}
}

// ----------------------------------------------------------------------

void CellPermissions::UpdateObserver::init(BuildingObject *building)
{
	if (!building || s_activeUpdaters.find(building) != s_activeUpdaters.end())
		return;

	m_building = building;
	s_activeUpdaters.insert(building);
	// get permissions for all observers for all cells of this building
	getCellPermissions(*building);
}

// ----------------------------------------------------------------------

void CellPermissions::UpdateObserver::getCellPermissions(ServerObject &obj)
{
	Container * const container = ContainerInterface::getContainer(obj);
	if (container)
	{
		for (ContainerIterator i = container->begin(); i != container->end(); ++i)
		{
			auto * const content = safe_cast<ServerObject*>((*i).getObject());
			if (content)
			{
				CellObject * const cell = content->asCellObject();
				if (cell)
				{
					std::set<Client *> const &observers = m_building->getObservers();
					for (auto observer : observers)
					{
						ServerObject * const so = observer->getCharacterObject();
						if (so)
						{
							CreatureObject * const creature = so->asCreatureObject();
							if (creature)
								m_permissions.push_back(cell->isAllowed(*creature));
						}
					}
				}
				getCellPermissions(*content);
			}
		}
	}
}

// ----------------------------------------------------------------------

void CellPermissions::UpdateObserver::handleCellPermissionsUpdateIfNeeded(ServerObject &obj, unsigned int &permPos, std::vector<CreatureObject*> &expel)
{
	Container *container = ContainerInterface::getContainer(obj);
	if (container)
	{
		for (ContainerIterator i = container->begin(); i != container->end(); ++i)
		{
			auto * const content = safe_cast<ServerObject *>((*i).getObject());
			if (content)
			{
				CellObject * const cell = content->asCellObject();
				if (cell)
				{
					std::set<Client *> const &observers = m_building->getObservers();
					for (auto observer : observers)
					{
						ServerObject * const so = observer->getCharacterObject();
						if (so)
						{
							CreatureObject * const creature = so->asCreatureObject();
							if (creature)
							{
								bool const allowed = cell->isAllowed(*creature);
								if (allowed != m_permissions[permPos])
								{
									LOG("CellPermUpdate", ("sending UpdateCellPermission for %s to client %s (%d)",
										cell->getNetworkId().getValueString().c_str(),
										observer->getCharacterObjectId().getValueString().c_str(),
										allowed ? 1 : 0));
									UpdateCellPermissionMessage const message(cell->getNetworkId(), allowed);
									observer->send(message, true);
								}
								if (!allowed && creature->getAttachedTo() == cell)
									expel.push_back(creature);
								++permPos;
							}
						}
					}
				}
				handleCellPermissionsUpdateIfNeeded(*content, permPos, expel);
			}
		}
	}
}

// ======================================================================

CellPermissions::ViewerChangeObserver::ViewerChangeObserver(CreatureObject *creature) :
	m_profilerBlock("CellPermissions::ViewerChangeObserver"),
	m_cells(),
	m_permissions(),
	m_creature(creature)
{
	if (creature)
	{
		Client * const client = creature->getClient();
		if (client)
		{
			Client::ObservingList const &observing = client->getObserving();
			for (auto i : observing)
			{
				CellObject * const cell = i->asCellObject();
				if (cell)
				{
					m_cells.push_back(cell);
					m_permissions.push_back(cell->isAllowed(*creature));
				}
			}
		}
	}
}

// ----------------------------------------------------------------------

CellPermissions::ViewerChangeObserver::~ViewerChangeObserver()
{
	if (!m_creature)
		return;

	// send any needed permission updates
	Client * const client = m_creature->getClient();
	if (client)
	{
		for (unsigned int i = 0; i < m_cells.size(); ++i)
		{
			bool const allowed = m_cells[i]->isAllowed(*m_creature);
			if (allowed != m_permissions[i] || m_creature->getClient()->isGod())
			{
				UpdateCellPermissionMessage const message(m_cells[i]->getNetworkId(), allowed);
				client->send(message, true);
			}
		}
	}

	// expel from the current building if they're not allowed in anymore
	CellObject * const cell = ContainerInterface::getContainingCellObject(*m_creature);
	if (cell && !cell->isAllowed(*m_creature) && !m_creature->getClient()->isGod())
	{
		BuildingObject * const building = cell->getOwnerBuilding();
		if (building)
			building->expelObject(*m_creature);
	}
}

// ======================================================================

bool CellPermissions::isOnList(PermissionList const &permList, std::string const &name) // static
{
	for (const auto & i : permList)
		if (!_stricmp(i.getName().c_str(), name.c_str()))
			return true;

	return false;
}

// ----------------------------------------------------------------------

bool CellPermissions::isOnList(PermissionList const &permList, CreatureObject const &who) // static
{
    if(who.getClient() && who.isPlayerControlled())
    {
        // always consider god mode to be on the list
        if (who.getClient()->isGod())
        {
            return true;
        }

        const int guildId = who.getGuildId();
        std::vector<int> const &cityIds = CityInterface::getCitizenOfCityId(who.getNetworkId());
        const int cityId = cityIds.empty() ? 0 : cityIds.front();
        const uint32 faction = who.getPvpFaction();
        const uint32 stationId = NameManager::getInstance().getPlayerStationId(who.getNetworkId());

        for (const auto &i : permList)
        {
            const std::string &name = i.getName();

            if (guildId > 0 && name.rfind("guild:", 0) == 0)
            {
                if (guildId == GuildInterface::findGuild(name.substr(6, name.length())))
                {
                    return true;
                }
            }
            if (cityId > 0 && name.rfind("city:", 0) == 0)
            {
                if (cityId == CityInterface::findCityByName(name.substr(5, name.length())))
                {
                    return true;
                }
            }
            if (faction != 0 && name.rfind("faction:", 0) == 0)
            {
                if (name.rfind("Imperial", 8) == 8)
                {
                    if (faction == PvpData::getImperialFactionId())
                    {
                        return true;
                    }
                }
                else if (name.rfind("Rebel", 8) == 8)
                {
                    if (faction == PvpData::getRebelFactionId())
                    {
                        return true;
                    }
                }
            }
            if (name.rfind("account:", 0) == 0)
            {
                if (CellPermissionsNamespace::parseBits32(name.substr(8, name.length())) == stationId)
                {
                    return true;
                }

            }
            if (name.rfind(Unicode::wideToNarrow(who.getAssignedObjectFirstName()), 0) == 0)
            {
                return true;
            }
        }
    }
    else
    {
        for (PermissionList::const_iterator i = permList.begin(); i != permList.end(); ++i)
        {
            const std::string& name = (*i).getName();
            if (name == who.getNetworkId().getValueString())
            {
                return true;
            }
            if (!_stricmp(name.c_str(), Unicode::wideToNarrow(who.getAssignedObjectFirstName()).c_str()))
            {
                return true;
            }
        }
    }
	return false;
}

// ======================================================================

