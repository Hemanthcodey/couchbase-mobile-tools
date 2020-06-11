//
// CBLiteTool.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "CBLiteTool.hh"
#include "CBLiteCommand.hh"
#include "StringUtil.hh"            // for digittoint(), on non-BSD-like systems

using namespace litecore;


int main(int argc, const char * argv[]) {
    CBLiteTool tool;
    return tool.main(argc, argv);
}


void CBLiteTool::usage() {
    cerr <<
    ansiBold() << "cblite: Couchbase Lite / LiteCore database multi-tool\n" << ansiReset() <<
    "Usage: cblite cat " << it("[FLAGS] DBPATH DOCID [DOCID...]") << "\n"
    "       cblite compact " << it("DBPATH") << "\n"
    "       cblite cp " << it("[FLAGS] SOURCE DESTINATION") << "\n"
#ifdef COUCHBASE_ENTERPRISE
    "       cblite decrypt " << it("DBPATH") << "\n"
    "       cblite encrypt " << it("[FLAGS] DBPATH") << "\n"
#endif
    "       cblite help " << it("[SUBCOMMAND]") << "\n"
    "       cblite info " << it("[FLAGS] DBPATH [indexes] [index NAME]") << "\n"
    "       cblite ls " << it("[FLAGS] DBPATH [PATTERN]") << "\n"
    "       cblite pull " << it("[FLAGS] DBPATH SOURCE") << "\n"
    "       cblite push " << it("[FLAGS] DBPATH DESTINATION") << "\n"
    "       cblite put " << it("[FLAGS] DBPATH DOCID \"JSON\"") << "\n"
    "       cblite query " << it("[FLAGS] DBPATH JSONQUERY") << "\n"
    "       cblite revs " << it("DBPATH DOCID") << "\n"
    "       cblite rm " << it("DBPATH DOCID") << "\n"
    "       cblite select " << it("[FLAGS] DBPATH N1QLQUERY") << "\n"
    "       cblite serve " << it("[FLAGS] DBPATH") << "\n"
//  "       cblite sql " << it("DBPATH QUERY") << "\n"
    "       cblite " << it("DBPATH   (interactive shell*)\n") <<
    "For information about subcommand parameters/flags, run `cblite help SUBCOMMAND`.\n"
    "\n"
    "* The shell accepts the same commands listed above, but without the 'cblite'\n"
    "  and DBPATH parameters. For example, 'ls -l'.\n"
    "\n"
    "Global flags (before the subcommand name):\n"
    "  --color : Use bold/italic (and sometimes color), if terminal supports it\n"
    "  --create : Creates the database if it doesn't already exist.\n"
    "  --encrypted : Open an encrypted database (will prompt for password from stdin)\n"
    "  --version : Display version info and exit\n"
    "  --writeable : Open the database with read+write access\n"
    "  --version or -v : Log version information and exit\n"
    ;
}


void CBLiteTool::writeUsageCommand(const char *cmd, bool hasFlags, const char *otherArgs) {
    cerr << ansiBold();
    if (!_interactive)
        cerr << "cblite ";
    cerr << cmd << ' ' << ansiItalic();
    if (hasFlags)
        cerr << "[FLAGS]" << ' ';
    if (!_interactive)
        cerr << "DBPATH ";
    cerr << otherArgs << ansiReset() << "\n";
}


void CBLiteTool::displayVersion() {
    alloc_slice version = c4_getVersion();
    cout << "Couchbase Lite Core " << version << "\n";
    exit(0);
}


int CBLiteTool::run() {
    processFlags({
        {"--create",    [&]{_dbFlags |= kC4DB_Create; _dbFlags &= ~kC4DB_ReadOnly;}},
        {"--writeable", [&]{_dbFlags &= ~kC4DB_ReadOnly;}},
        {"--encrypted", [&]{_dbNeedsPassword = true;}},
        {"--version",   [&]{displayVersion();}},
        {"-v",          [&]{displayVersion();}},
    });

    c4log_setCallbackLevel(kC4LogWarning);
    if (!hasArgs()) {
        cerr << ansiBold()
             << "cblite: Couchbase Lite / LiteCore database multi-tool\n" << ansiReset() 
             << "Missing subcommand or database path.\n"
             << "For a list of subcommands, run " << ansiBold() << "cblite help" << ansiReset() << ".\n"
             << "To start the interactive mode, run "
             << ansiBold() << "cblite " << ansiItalic() << "DBPATH" << ansiReset() << '\n';
        fail();
    }

    string cmd = nextArg("subcommand or database path");
    if (isDatabasePath(cmd)) {
        endOfArgs();
        _interactive = true;
        openDatabase(cmd);
        runInteractively();
    } else {
        if (cmd == "help") {
            helpCommand();
        } else {
            auto subcommandInstance = subcommand(cmd);
            if (subcommandInstance) {
                subcommandInstance->runSubcommand();
            } else {
                if (cmd.find(FilePath::kSeparator) != string::npos || cmd.find('.') || cmd.size() > 10)
                    fail(format("Not a valid database path (must end in %s) or subcommand name: %s",
                                kC4DatabaseFilenameExtension, cmd.c_str()));
                else
                    failMisuse(format("Unknown subcommand '%s'", cmd.c_str()));
            }
        }
    }
    return 0;
}


bool CBLiteTool::isDatabasePath(const string &path) {
    return hasSuffix(FilePath(path).fileOrDirName(), kC4DatabaseFilenameExtension);
}


#if COUCHBASE_ENTERPRISE
static bool setHexKey(C4EncryptionKey *key, const string &str) {
    if (str.size() != 2 * kC4EncryptionKeySizeAES256)
        return false;
    uint8_t *dst = &key->bytes[0];
    for (size_t src = 0; src < 2 * kC4EncryptionKeySizeAES256; src += 2) {
        if (!isxdigit(str[src]) || !isxdigit(str[src+1]))
            return false;
        *dst++ = (uint8_t)(16*digittoint(str[src]) + digittoint(str[src+1]));
    }
    key->algorithm = kC4EncryptionAES256;
    return true;
}
#endif


void CBLiteTool::openDatabase(string path) {
    fixUpPath(path);
    if (!isDatabasePath(path))
        fail("Database filename must have a '.cblite2' extension");
    C4DatabaseConfig config = {_dbFlags};
    C4Error err;
    const C4Error kEncryptedDBError = {LiteCoreDomain, kC4ErrorNotADatabaseFile};

    if (!_dbNeedsPassword) {
        _db = c4db_open(c4str(path), &config, &err);
    } else {
        // If --encrypted flag given, skip opening db as unencrypted
        err = kEncryptedDBError;
    }

    while (!_db && err == kEncryptedDBError) {
#ifdef COUCHBASE_ENTERPRISE
        // Database is encrypted
        if (!_interactive && !_dbNeedsPassword) {
            // Don't prompt for a password unless this is an interactive session
            fail("Database is encrypted (use `--encrypted` flag to get a password prompt)");
        }
        const char *prompt = "Database password or hex key:";
        if (config.encryptionKey.algorithm != kC4EncryptionNone)
            prompt = "Sorry, try again: ";
        string password = readPassword(prompt);
        if (password.empty())
            exit(1);
        if (!setHexKey(&config.encryptionKey, password)
                && !c4key_setPassword(&config.encryptionKey, slice(password), kC4EncryptionAES256)) {
            cout << "Error: Couldn't derive key from password\n";
            continue;
        }
        _db = c4db_open(c4str(path), &config, &err);
#else
        fail("Database is encrypted (Enterprise Edition is required to open encrypted databases)");
#endif
    }
    
    if (!_db)
        fail(format("Couldn't open database %s", path.c_str()), err);
}


void CBLiteTool::openDatabaseFromNextArg() {
    if (!_db)
        openDatabase(nextArg("database path"));
}


void CBLiteTool::openWriteableDatabaseFromNextArg() {
    if (_db) {
        if (_dbFlags & kC4DB_ReadOnly)
            fail("Database was opened read-only; run `cblite --writeable` to allow writes");
    } else {
        _dbFlags &= ~kC4DB_ReadOnly;
        openDatabaseFromNextArg();
    }
}


#pragma mark - INTERACTIVE MODE:


void CBLiteTool::shell() {
    // Read params:
    openDatabaseFromNextArg();
    endOfArgs();
    runInteractively();
}


void CBLiteTool::runInteractively() {
    _interactive = true;
    const char *mode = (_dbFlags & kC4DB_ReadOnly) ? "read-only" : "writeable";
    cout << "Opened " << mode << " database " << alloc_slice(c4db_getPath(_db)) << '\n';

    while(true) {
        try {
            if (!readLine("(cblite) "))
                return;
            string cmd = nextArg("subcommand");
            if (cmd == "help") {
                helpCommand();
            } else {
                auto subcommandInstance = subcommand(cmd);
                if (subcommandInstance)
                    subcommandInstance->runSubcommand();
                else
                    cerr << format("Unknown subcommand '%s'; type 'help' for a list of commands.\n",
                                   cmd.c_str());
            }
        } catch (const exit_error &) {
            // subcommand exited; continue
        } catch (const fail_error &) {
            // subcommand failed (error message was already printed); continue
        }
    }
}


void CBLiteTool::helpCommand() {
    if (hasArgs()) {
        string currentCommand = nextArg("subcommand");
        auto subcommandInstance = this->subcommand(currentCommand);
        if (subcommandInstance)
            subcommandInstance->usage();
        else
            cerr << format("Unknown subcommand '%s'\n", currentCommand.c_str());

    } else if (_interactive) {
        cout << bold("Subcommands:") << "\n" <<
        "    cat " << it("[FLAGS] DOCID [DOCID...]") << "\n"
        "    compact\n"
        "    cp " << it("[FLAGS] DESTINATION") << "\n"
#ifdef COUCHBASE_ENTERPRISE
        "    decrypt\n"
        "    encrypt " << it("[FLAGS]") << "\n"
#endif
        "    help " << it("[SUBCOMMAND]") << "\n"
        "    info " << it("[FLAGS] [indexes] [index NAME]") << "\n"
        "    ls " << it("[FLAGS] [PATTERN]") << "\n"
        "    pull " << it("[FLAGS] SOURCE") << "\n"
        "    push " << it("[FLAGS] DESTINATION") << "\n"
        "    put " << it("[FLAGS] DOCID JSON_BODY") << "\n"
        "    query " << it("[FLAGS] JSON_QUERY") << "\n"
        "    revs " << it("DOCID") << "\n"
        "    rm " << it("DOCID") << "\n"
        "    select " << it("[FLAGS] N1QLQUERY") << "\n"
        "    serve " << it("[FLAGS]") << "\n"
    //  "    sql " << it("QUERY") << "\n"
        "For more details, enter `help` followed by a subcommand name.\n"
        ;
    } else {
        usage();
    }
}


void CBLiteTool::quitCommand() {
    if (_db)
        c4db_close(_db, nullptr);
    exit(0);
}


unique_ptr<CBLiteCommand> CBLiteTool::subcommand(const string &name) {
    CBLiteCommand* (*factory)(CBLiteTool&) = nullptr;
    processFlag(name, {
        {"cat",     [&]{factory = &newCatCommand;}},
        {"compact", [&]{factory = newCompactCommand;}},
        {"cp",      [&]{factory = newCpCommand;}},
        {"export",  [&]{factory = newExportCommand;}},
        {"file",    [&]{factory = newInfoCommand;}},
        {"import",  [&]{factory = newImportCommand;}},
        {"info",    [&]{factory = newInfoCommand;}},
        {"ls",      [&]{factory = newListCommand;}},
        {"pull",    [&]{factory = newPullCommand;}},
        {"push",    [&]{factory = newPushCommand;}},
        {"put",     [&]{factory = newPutCommand;}},
        {"query",   [&]{factory = newQueryCommand;}},
        {"revs",    [&]{factory = newRevsCommand;}},
        {"rm",      [&]{factory = newRmCommand;}},
        {"SELECT",  [&]{factory = newSelectCommand;}},
        {"select",  [&]{factory = newSelectCommand;}},
        {"sql",     [&]{factory = newSQLCommand;}},
        {"serve",   [&]{if (!_interactive) factory = newServeCommand;}},
        {"quit",    [&]{if (_interactive) quitCommand();}},
#ifdef COUCHBASE_ENTERPRISE
        {"decrypt", [&]{factory = newDecryptCommand;}},
        {"encrypt", [&]{factory = newEncryptCommand;}},
#endif
    });

    if (!factory)
        return nullptr;
    unique_ptr<CBLiteCommand> command( factory(*this) );
    command->setName(name);
    return command;
}
