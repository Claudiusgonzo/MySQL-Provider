/*--------------------------------------------------------------------------------
 *        Copyright (c) Microsoft Corporation. All rights reserved. See license.txt for license information.
 *      
 *           */
 /**
        \file        sqlauth.cpp

        \brief       MySQL authentication handling for MySQL provider

        \date        11-13-14
*/
/*----------------------------------------------------------------------------*/

#include <scxcorelib/scxcmn.h>
#include <scxcorelib/scxconfigfile.h>
#include <scxcorelib/scxfilesystem.h>
#include <util/Base64Helper.h>

#include "sqlauth.h"


using namespace SCXCoreLib;

MySQL_Authentication::MySQL_Authentication(MySQL_AuthenticationDependencies* deps /* = new MySQL_AuthenticationDependencies() */)
    : m_deps(deps),
      m_config(deps->GetDefaultAuthFileName()),
      m_log(SCXCoreLib::SCXLogHandleFactory::GetLogHandle(L"mysql.authentication"))
{
}

void MySQL_Authentication::Load()
{
    try {
        m_config.LoadConfig();
    }
    catch (SCXFilePathNotFoundException e)
    {
        // Fall through - empty configuraiton is okay
    }

    // Read in the default entry if it exists
    if ( m_config.KeyExists(L"0") )
    {
        GetEntry(0, m_default);
    }
}

void MySQL_Authentication::Save()
{
    m_config.SaveConfig();
}

void MySQL_Authentication::AllowAutomaticUpdates(bool bAllowed)
{
    std::wstring setting(bAllowed ? L"true" : L"false");
    m_config.SetValue(L"AutoUpdate", setting);
}

void MySQL_Authentication::AddCredentialSet(unsigned int port, const std::string& binding,
                                            const std::string& username, const std::string& password)
{
    if ( ! username.empty() && password.empty() )
    {
        throw SCXInvalidArgumentException(L"user", L"no password specified with a username", SCXSRCLOCATION);
    }

    if ( username.empty() && ! password.empty() )
    {
        throw SCXInvalidArgumentException(L"password", L"no username specified with a password", SCXSRCLOCATION);
    }

    if ( 0 == port )
    {
        // Special behavior for default: Allow only binding, or only username/password, or both

        m_config.SetValue(L"0", FormatPortSpecification(binding, username, password));

        // Save the default values
        m_default.port = 0;
        m_default.binding = binding;
        m_default.username = username;
        m_default.password = password;
        return;
    }

    if ( binding.empty() && m_default.binding.empty() )
    {
        throw MySQL_Auth::InvalidAuthentication(L"no binding host specified", SCXSRCLOCATION);
    }

    if ( username.empty() && m_default.username.empty() )
    {
        throw MySQL_Auth::InvalidAuthentication(L"no username specified", SCXSRCLOCATION);
    }

    if ( password.empty() && m_default.password.empty() )
    {
        throw MySQL_Auth::InvalidAuthentication(L"no password specified", SCXSRCLOCATION);
    }

    // We have something that seems to work - store it
    m_config.SetValue(StrFrom(port), FormatPortSpecification(binding, username, password));
}

void MySQL_Authentication::DeleteCredentialSet(unsigned int port)
{
    // Does this port even exist in our configuration?
    if ( ! m_config.KeyExists(StrFrom(port)) )
    {
        throw SCXInvalidArgumentException(L"port", L"specified port does not exist in configuration", SCXSRCLOCATION);
    }

    // If there even is a default record and we're trying to delete it, then check all of
    // the authentication entries to be sure nobody is dependent on the default record

    if ( 0 == port && (! m_default.binding.empty() || ! m_default.username.empty() || ! m_default.password.empty()) )
    {
        // Check if default record required for anything
        for (std::map<std::wstring,std::wstring>::const_iterator it = m_config.begin(); it != m_config.end(); ++it)
        {
            // Skip keys that aren't port numbers - and skip the default port too
            if ( it->first == L"0" || std::string::npos != it->first.find_first_not_of(L"0123456789") )
            {
                continue;
            }

            // Convert the value to something meaningful (don't bother decoding the password)
            MySQL_AuthenticationEntry entry;
            std::vector<std::wstring> elements;
            StrTokenize( it->second, elements, L",", true, true );
            if ( elements.size() != 3 )
            {
                std::wstringstream ss;
                ss << L"Corrupt configuration, invalid value for port " << it->first << L": " << it->second;
                throw MySQL_Auth::InvalidAuthentication(ss.str(), SCXSRCLOCATION);
            }

            entry.port = StrToUInt(it->first);
            entry.binding = StrToUTF8( elements[0] );
            entry.username = StrToUTF8( elements[1] );
            entry.password = StrToUTF8( elements[2] );

            // Is the default record required for this entry?
            if ( (entry.binding.empty() && ! m_default.binding.empty())
                 || (entry.username.empty() && ! m_default.username.empty())
                 || (entry.password.empty() && ! m_default.password.empty()) )
            {
                throw MySQL_Auth::InvalidAuthentication(L"Default record is required for port " + it->first,
                                                        SCXSRCLOCATION);
            }
        }
    }

    m_config.DeleteEntry( StrFrom(port) );
}

bool MySQL_Authentication::GetAutomaticUpdates()
{
    // Default to "true" unless explicitly disabled
    bool fAutoUpdate = true;

    const std::wstring strKeyName( L"AutoUpdate" );
    if ( m_config.KeyExists(strKeyName) )
    {
        std::wstring value;
        m_config.GetValue(strKeyName, value);

        if ( 0 == StrCompare(value, L"true", true) || 0 == StrCompare(value, L"1") )
        {
            // AutoUpdate is enabled, so just fall through
        }
        else if ( 0 == StrCompare(value, L"false", true) || 0 == StrCompare(value, L"0") )
        {
            fAutoUpdate = false;
        }
        else
        {
            SCX_LOGWARNING(m_log, StrAppend(L"AutoUpdate value is not true or false, assuming true; value=", value));
        }
    }

    return fAutoUpdate;
}

void MySQL_Authentication::GetPortList(std::vector<unsigned int>& portList)
{
    portList.clear();

    for (std::map<std::wstring,std::wstring>::const_iterator it = m_config.begin(); it != m_config.end(); ++it)
    {
        // Skip keys that aren't port numbers - and skip the default port too
        if ( it->first == L"0" || std::string::npos != it->first.find_first_not_of(L"0123456789") )
        {
            continue;
        }

        portList.push_back( StrToUInt(it->first) );
    }
}

void MySQL_Authentication::GetEntry(const unsigned int& port, MySQL_AuthenticationEntry& entry)
{
    std::wstring value;
    if ( !m_config.GetValue(StrFrom(port), value) )
    {
        throw MySQL_Auth::PortNotFound(port, SCXSRCLOCATION);
    }

    entry.clear();
    std::vector<std::wstring> elements;
    StrTokenize( value, elements, L",", true, true );
    if ( elements.size() != 3 )
    {
        std::wstringstream ss;
        ss << L"invalid value for port " << StrFrom(port) << L": " << value;
        throw MySQL_Auth::InvalidAuthentication(ss.str(), SCXSRCLOCATION);
    }

    entry.port = port;
    entry.binding = StrToUTF8( elements[0] );
    entry.username = StrToUTF8( elements[1] );
    if ( ! elements[2].empty() )
    {
        std::string password;
        if ( ! util::Base64Helper::Decode(StrToUTF8(elements[2]), password) )
        {
            std::wstringstream ss;
            ss << L"unable to decode password for port " << port << L": " << value;
            throw MySQL_Auth::InvalidAuthentication(ss.str(), SCXSRCLOCATION);
        }
        entry.password = password;
    }

    if ( 0 != port )
    {
        // Deal with default values
        if ( entry.binding.empty() )
        {
            entry.binding = m_default.binding;
        }

        if ( entry.username.empty() )
        {
            entry.username = m_default.username;
        }

        if ( entry.password.empty() )
        {
            entry.password = m_default.password;
        }

        // Be sure everything we need is returned
        if ( entry.binding.empty() || entry.username.empty() || entry.password.empty() )
        {
            std::wstringstream ss;
            ss << L"missing values for port " << port << L": \"" << value << L"\"  "
               << L"Default binding: " << StrFromUTF8(m_default.binding)
               << L", default username: " << StrFromUTF8(m_default.username)
               << L", default password: " << ( ! m_default.password.empty() ? L"not empty" : L"empty" );
            throw MySQL_Auth::InvalidAuthentication(ss.str(), SCXSRCLOCATION);
        }
    }
}

std::wstring MySQL_Authentication::FormatPortSpecification(
    const std::string& binding, const std::string& user, const std::string& password)
{
    std::string b64Password;
    util::Base64Helper::Encode(password, b64Password);

    return StrFromUTF8(binding + ", " + user + ", " + b64Password);
}


/*----------------------------E-N-D---O-F---F-I-L-E---------------------------*/
