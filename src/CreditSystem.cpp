#include "CreditSystem.h"
#include "Server.h"
#include "Utils.h"

namespace stdrave {

bool createTable(sqlite3* db)
{
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS key_credits (
            api_key TEXT PRIMARY KEY,
            consumed REAL,
            credits_limit INTEGER,
            key_flags INTEGER
        );
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating table: " << (errMsg ? errMsg : "unknown") << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool storeValues(sqlite3* db, const std::string& apiKey, float creditsConsumed, uint32_t creditsLimit, APIKeyFlags keyFlags)
{
    const char* sql = R"SQL(
        INSERT OR REPLACE INTO key_credits (api_key, consumed, credits_limit, key_flags)
        VALUES (?, ?, ?, ?);
    )SQL";

    sqlite3_stmt* stmt = nullptr;

    // Prepare the statement
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    // Bind parameters
    // 1-based indexing in SQLite3 bind calls
    sqlite3_bind_text(stmt, 1, apiKey.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, static_cast<double>(creditsConsumed));
    sqlite3_bind_int(stmt, 3, static_cast<int>(creditsLimit));
    sqlite3_bind_int(stmt, 4, static_cast<int>(keyFlags));

    // Execute
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "Error inserting data: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    // Finalize
    sqlite3_finalize(stmt);
    return true;
}

bool getValues(sqlite3* db, const std::string& apiKey, float& creditsConsumed, uint32_t& creditsLimit, APIKeyFlags& keyFlags)
{
    const char* sql = R"SQL(
        SELECT consumed, credits_limit, key_flags
        FROM key_credits
        WHERE api_key = ?;
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    // Bind the apiKey
    sqlite3_bind_text(stmt, 1, apiKey.c_str(), -1, SQLITE_TRANSIENT);

    // Execute (once)
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        
        // We have a row
        creditsConsumed = static_cast<float>(sqlite3_column_double(stmt, 0));
        creditsLimit  = static_cast<uint32_t>(sqlite3_column_int(stmt, 1));
        keyFlags  = static_cast<APIKeyFlags>(sqlite3_column_int(stmt, 2));

        sqlite3_finalize(stmt);
        return true;
    }
    else if (rc == SQLITE_DONE) {
        // No results
        std::cerr << "No row found for apiKey: " << apiKey << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }
    else {
        // Error
        std::cerr << "Error stepping through result: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }
}

bool consumeCredits(sqlite3* db, const std::string& apiKey, float credits)
{
    // SQL: increment the "consumed" column by a certain amount for this apiKey
    const char* sql = R"SQL(
        UPDATE key_credits
        SET consumed = consumed + ?
        WHERE api_key = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Error preparing statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }

    // Bind parameters
    // 1) The credits to add
    sqlite3_bind_double(stmt, 1, static_cast<double>(credits));
    // 2) The apiKey
    sqlite3_bind_text(stmt, 2, apiKey.c_str(), -1, SQLITE_TRANSIENT);

    // Execute the UPDATE
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        std::cerr << "Error updating consumed: " << sqlite3_errmsg(db) << std::endl;
        sqlite3_finalize(stmt);
        return false;
    }

    // Check how many rows were actually changed
    int changedRows = sqlite3_changes(db);

    sqlite3_finalize(stmt);

    // If changedRows == 0, it means apiKey wasn't found (or consumed didn't change, but here it
    // should always change if the row exists, so 0 means no row matched).
    return (changedRows > 0);
}

bool createKeyCreditsTable(sqlite3* db)
{
    const char* sql = R"SQL(
        CREATE TABLE IF NOT EXISTS key_credits (
            api_key TEXT PRIMARY KEY,
            consumed REAL,
            credits_limit INTEGER,
            key_flags INTEGER
        );
    )SQL";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating table: " << (errMsg ? errMsg : "unknown") << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

bool Server::startDatabse()
{
#if 0
    std::string key5 = generateApiKey();
    std::cout << "key5: " << key5 << std::endl;
    std::string key10 = generateApiKey();
    std::cout << "key10: " << key10 << std::endl;
    std::string key20 = generateApiKey();
    std::cout << "key20: " << key20 << std::endl;
    std::string key50 = generateApiKey();
    std::cout << "key50: " << key50 << std::endl;
    std::string key100 = generateApiKey();
    std::cout << "key100: " << key100 << std::endl;
#endif
    
    m_db = nullptr;
    
    std::string dbPath = getEnvironmentDir() + "/" + PRODUCT_NAME + ".db";
    int rc = sqlite3_open(dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK || !m_db) {
        if(m_db)
        {
            std::cerr << "Cannot open database: " << sqlite3_errmsg(m_db) << std::endl;
        }
        else
        {
            std::cerr << "Cannot open database" << std::endl;
        }
        return false;
    }
    
    createKeyCreditsTable(m_db);
    
#if 0
    APIKeyFlags keyFlags = APIKeyFlags::APIKey_manual;
    bool ok = storeValues(m_db, "gkr_e56e4188-fe0e-431d-a881-0f295ae90034", 0, 5, keyFlags);
    ok = storeValues(m_db, "gkr_14461acb-a53d-417e-b9d5-c8481763f654", 0, 10, keyFlags);
    ok = storeValues(m_db, "gkr_3dcf69da-edfa-4ff8-bace-293c7b02b875", 0, 20, keyFlags);
    ok = storeValues(m_db, "gkr_37b4b0c7-2773-4670-806a-0aa07996592a", 0, 50, keyFlags);
    ok = storeValues(m_db, "gkr_cd71bcc0-0767-4580-8591-af05376dcbf2", 0, 100, keyFlags);
#endif
    
    return true;
}

// Returns "" if valid (no error).
// Otherwise returns a message describing the issue.
std::string isAPIKeyValid(sqlite3* db, const std::string& apiKey)
{
    // We only need columns "consumed" and "credits_limit"
    const char* sql = R"SQL(
        SELECT consumed, credits_limit
        FROM key_credits
        WHERE api_key = ?
    )SQL";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = "Error preparing statement: ";
        err += sqlite3_errmsg(db);
        return err;
    }

    // Bind the apiKey
    rc = sqlite3_bind_text(stmt, 1, apiKey.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK) {
        std::string err = "Error binding apiKey: ";
        err += sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return err;
    }

    // Step through results (there should be at most one row)
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        // We have a row => check the columns
        float consumed     = static_cast<float>(sqlite3_column_double(stmt, 0));
        int creditsLimit   = sqlite3_column_int(stmt, 1);  // or cast to uint32_t

        sqlite3_finalize(stmt);

        // Check if consumed >= creditsLimit
        if (consumed >= static_cast<float>(creditsLimit)) {
            return "Insufficient credits";
        }
        else {
            // Valid: row found, consumed < limit
            return "";
        }
    }
    else if (rc == SQLITE_DONE)
    {
        // No row found
        sqlite3_finalize(stmt);
        return "API Key not found";
    }
    else
    {
        // Some unexpected error
        std::string err = "Error stepping through result: ";
        err += sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return err;
    }
}

bool Server::shutdownDatabase()
{
    if(m_db)
    {
        sqlite3_close(m_db);
    }
    return true;
}

}
