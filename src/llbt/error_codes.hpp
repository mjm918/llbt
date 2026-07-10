/*************************************************************************
 *
 * Copyright 2021 Realm Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 **************************************************************************/

#pragma once

#include <cstdint>
#include <type_traits>
#include <string>
#include <vector>
#include <llbt/error_codes.h>

namespace llbt {

// ErrorExtraInfo subclasses:

struct ErrorCategory {
    enum Type {
        logic_error = LLBT_ERR_CAT_LOGIC,
        runtime_error = LLBT_ERR_CAT_RUNTIME,
        invalid_argument = LLBT_ERR_CAT_INVALID_ARG,
        file_access = LLBT_ERR_CAT_FILE_ACCESS,
        system_error = LLBT_ERR_CAT_SYSTEM_ERROR,
        app_error = LLBT_ERR_CAT_APP_ERROR,
        client_error = LLBT_ERR_CAT_CLIENT_ERROR,
        json_error = LLBT_ERR_CAT_JSON_ERROR,
        service_error = LLBT_ERR_CAT_SERVICE_ERROR,
        http_error = LLBT_ERR_CAT_HTTP_ERROR,
        custom_error = LLBT_ERR_CAT_CUSTOM_ERROR,
        websocket_error = LLBT_ERR_CAT_WEBSOCKET_ERROR,
        sync_error = LLBT_ERR_CAT_SYNC_ERROR,
    };
    constexpr ErrorCategory() = default;
    constexpr bool test(Type cat)
    {
        return (m_value & cat) != 0;
    }
    constexpr ErrorCategory& set(Type cat)
    {
        m_value |= cat;
        return *this;
    }
    constexpr void reset(Type cat)
    {
        m_value &= ~cat;
    }
    constexpr bool operator==(const ErrorCategory& other) const
    {
        return m_value == other.m_value;
    }
    constexpr bool operator!=(const ErrorCategory& other) const
    {
        return m_value != other.m_value;
    }
    constexpr int value() const
    {
        return m_value;
    }

private:
    unsigned m_value = 0;
};

class ErrorCodes {
public:
    // Explicitly 32-bits wide so that non-symbolic values,
    // like uassert codes, are valid.
    enum Error : std::int32_t {
        OK = LLBT_ERR_NONE,
        RuntimeError = LLBT_ERR_RUNTIME,
        RangeError = LLBT_ERR_RANGE_ERROR,
        BrokenInvariant = LLBT_ERR_BROKEN_INVARIANT,
        OutOfMemory = LLBT_ERR_OUT_OF_MEMORY,
        OutOfDiskSpace = LLBT_ERR_OUT_OF_DISK_SPACE,
        AddressSpaceExhausted = LLBT_ERR_ADDRESS_SPACE_EXHAUSTED,
        MaximumFileSizeExceeded = LLBT_ERR_MAXIMUM_FILE_SIZE_EXCEEDED,
        IncompatibleSession = LLBT_ERR_INCOMPATIBLE_SESSION,
        IncompatibleLockFile = LLBT_ERR_INCOMPATIBLE_LOCK_FILE,
        UnsupportedFileFormatVersion = LLBT_ERR_UNSUPPORTED_FILE_FORMAT_VERSION,
        MultipleSyncAgents = LLBT_ERR_MULTIPLE_SYNC_AGENTS,
        ObjectAlreadyExists = LLBT_ERR_OBJECT_ALREADY_EXISTS,
        NotCloneable = LLBT_ERR_NOT_CLONABLE,
        BadChangeset = LLBT_ERR_BAD_CHANGESET,
        SubscriptionFailed = LLBT_ERR_SUBSCRIPTION_FAILED,
        FileOperationFailed = LLBT_ERR_FILE_OPERATION_FAILED,
        PermissionDenied = LLBT_ERR_FILE_PERMISSION_DENIED,
        FileNotFound = LLBT_ERR_FILE_NOT_FOUND,
        FileAlreadyExists = LLBT_ERR_FILE_ALREADY_EXISTS,
        InvalidDatabase = LLBT_ERR_INVALID_DATABASE,
        DecryptionFailed = LLBT_ERR_DECRYPTION_FAILED,
        IncompatibleHistories = LLBT_ERR_INCOMPATIBLE_HISTORIES,
        FileFormatUpgradeRequired = LLBT_ERR_FILE_FORMAT_UPGRADE_REQUIRED,
        SchemaVersionMismatch = LLBT_ERR_SCHEMA_VERSION_MISMATCH,
        NoSubscriptionForWrite = LLBT_ERR_NO_SUBSCRIPTION_FOR_WRITE,
        BadVersion = LLBT_ERR_BAD_VERSION,
        OperationAborted = LLBT_ERR_OPERATION_ABORTED,

        AutoClientResetFailed = LLBT_ERR_AUTO_CLIENT_RESET_FAILED,
        BadSyncPartitionValue = LLBT_ERR_BAD_SYNC_PARTITION_VALUE,
        ConnectionClosed = LLBT_ERR_CONNECTION_CLOSED,
        InvalidSubscriptionQuery = LLBT_ERR_INVALID_SUBSCRIPTION_QUERY,
        SyncClientResetRequired = LLBT_ERR_SYNC_CLIENT_RESET_REQUIRED,
        SyncCompensatingWrite = LLBT_ERR_SYNC_COMPENSATING_WRITE,
        SyncConnectFailed = LLBT_ERR_SYNC_CONNECT_FAILED,
        SyncConnectTimeout = LLBT_ERR_SYNC_CONNECT_TIMEOUT,
        SyncInvalidSchemaChange = LLBT_ERR_SYNC_INVALID_SCHEMA_CHANGE,
        SyncPermissionDenied = LLBT_ERR_SYNC_PERMISSION_DENIED,
        SyncProtocolInvariantFailed = LLBT_ERR_SYNC_PROTOCOL_INVARIANT_FAILED,
        SyncProtocolNegotiationFailed = LLBT_ERR_SYNC_PROTOCOL_NEGOTIATION_FAILED,
        SyncServerPermissionsChanged = LLBT_ERR_SYNC_SERVER_PERMISSIONS_CHANGED,
        SyncUserMismatch = LLBT_ERR_SYNC_USER_MISMATCH,
        TlsHandshakeFailed = LLBT_ERR_TLS_HANDSHAKE_FAILED,
        WrongSyncType = LLBT_ERR_WRONG_SYNC_TYPE,
        SyncWriteNotAllowed = LLBT_ERR_SYNC_WRITE_NOT_ALLOWED,
        SyncLocalClockBeforeEpoch = LLBT_ERR_SYNC_LOCAL_CLOCK_BEFORE_EPOCH,
        SyncSchemaMigrationError = LLBT_ERR_SYNC_SCHEMA_MIGRATION_ERROR,

        SystemError = LLBT_ERR_SYSTEM_ERROR,

        LogicError = LLBT_ERR_LOGIC,
        NotSupported = LLBT_ERR_NOT_SUPPORTED,
        BrokenPromise = LLBT_ERR_BROKEN_PROMISE,
        CrossTableLinkTarget = LLBT_ERR_CROSS_TABLE_LINK_TARGET,
        KeyAlreadyUsed = LLBT_ERR_KEY_ALREADY_USED,
        WrongTransactionState = LLBT_ERR_WRONG_TRANSACTION_STATE,
        WrongThread = LLBT_ERR_WRONG_THREAD,
        IllegalOperation = LLBT_ERR_ILLEGAL_OPERATION,
        SerializationError = LLBT_ERR_SERIALIZATION_ERROR,
        StaleAccessor = LLBT_ERR_STALE_ACCESSOR,
        InvalidatedObject = LLBT_ERR_INVALIDATED_OBJECT,
        ReadOnlyDB = LLBT_ERR_READ_ONLY_DB,
        DeleteOnOpenBarq = LLBT_ERR_DELETE_OPENED_BARQ,
        MismatchedConfig = LLBT_ERR_MISMATCHED_CONFIG,
        ClosedBarq = LLBT_ERR_CLOSED_BARQ,
        InvalidTableRef = LLBT_ERR_INVALID_TABLE_REF,
        SchemaValidationFailed = LLBT_ERR_SCHEMA_VALIDATION_FAILED,
        SchemaMismatch = LLBT_ERR_SCHEMA_MISMATCH,
        InvalidSchemaVersion = LLBT_ERR_INVALID_SCHEMA_VERSION,
        InvalidSchemaChange = LLBT_ERR_INVALID_SCHEMA_CHANGE,
        MigrationFailed = LLBT_ERR_MIGRATION_FAILED,
        InvalidQuery = LLBT_ERR_INVALID_QUERY,

        BadServerUrl = LLBT_ERR_BAD_SERVER_URL,
        InvalidArgument = LLBT_ERR_INVALID_ARGUMENT,
        TypeMismatch = LLBT_ERR_PROPERTY_TYPE_MISMATCH,
        PropertyNotNullable = LLBT_ERR_PROPERTY_NOT_NULLABLE,
        ReadOnlyProperty = LLBT_ERR_READ_ONLY_PROPERTY,
        MissingPropertyValue = LLBT_ERR_MISSING_PROPERTY_VALUE,
        MissingPrimaryKey = LLBT_ERR_MISSING_PRIMARY_KEY,
        UnexpectedPrimaryKey = LLBT_ERR_UNEXPECTED_PRIMARY_KEY,
        ModifyPrimaryKey = LLBT_ERR_MODIFY_PRIMARY_KEY,
        SyntaxError = LLBT_ERR_INVALID_QUERY_STRING,
        InvalidProperty = LLBT_ERR_INVALID_PROPERTY,
        InvalidName = LLBT_ERR_INVALID_NAME,
        InvalidDictionaryKey = LLBT_ERR_INVALID_DICTIONARY_KEY,
        InvalidDictionaryValue = LLBT_ERR_INVALID_DICTIONARY_VALUE,
        InvalidSortDescriptor = LLBT_ERR_INVALID_SORT_DESCRIPTOR,
        InvalidEncryptionKey = LLBT_ERR_INVALID_ENCRYPTION_KEY,
        InvalidQueryArg = LLBT_ERR_INVALID_QUERY_ARG,
        KeyNotFound = LLBT_ERR_NO_SUCH_OBJECT,
        OutOfBounds = LLBT_ERR_INDEX_OUT_OF_BOUNDS,
        LimitExceeded = LLBT_ERR_LIMIT_EXCEEDED,
        ObjectTypeMismatch = LLBT_ERR_OBJECT_TYPE_MISMATCH,
        NoSuchTable = LLBT_ERR_NO_SUCH_TABLE,
        TableNameInUse = LLBT_ERR_TABLE_NAME_IN_USE,
        IllegalCombination = LLBT_ERR_ILLEGAL_COMBINATION,
        TopLevelObject = LLBT_ERR_TOP_LEVEL_OBJECT,

        CustomError = LLBT_ERR_CUSTOM_ERROR,

        ClientUserNotFound = LLBT_ERR_CLIENT_USER_NOT_FOUND,
        ClientUserNotLoggedIn = LLBT_ERR_CLIENT_USER_NOT_LOGGED_IN,
        ClientUserAlreadyNamed = LLBT_ERR_CLIENT_USER_ALREADY_NAMED,
        ClientRedirectError = LLBT_ERR_CLIENT_REDIRECT_ERROR,
        ClientTooManyRedirects = LLBT_ERR_CLIENT_TOO_MANY_REDIRECTS,

        BadToken = LLBT_ERR_BAD_TOKEN,
        MalformedJson = LLBT_ERR_MALFORMED_JSON,
        MissingJsonKey = LLBT_ERR_MISSING_JSON_KEY,
        BadJsonParse = LLBT_ERR_BAD_JSON_PARSE,

        MissingAuthReq = LLBT_ERR_MISSING_AUTH_REQ,
        InvalidSession = LLBT_ERR_INVALID_SESSION,
        UserAppDomainMismatch = LLBT_ERR_USER_APP_DOMAIN_MISMATCH,
        DomainNotAllowed = LLBT_ERR_DOMAIN_NOT_ALLOWED,
        ReadSizeLimitExceeded = LLBT_ERR_READ_SIZE_LIMIT_EXCEEDED,
        InvalidParameter = LLBT_ERR_INVALID_PARAMETER,
        MissingParameter = LLBT_ERR_MISSING_PARAMETER,
        TwilioError = LLBT_ERR_TWILIO_ERROR,
        GCMError = LLBT_ERR_GCM_ERROR,
        HTTPError = LLBT_ERR_HTTP_ERROR,
        AWSError = LLBT_ERR_AWS_ERROR,
        ServiceError = LLBT_ERR_SERVICE_ERROR,
        ArgumentsNotAllowed = LLBT_ERR_ARGUMENTS_NOT_ALLOWED,
        FunctionExecutionError = LLBT_ERR_FUNCTION_EXECUTION_ERROR,
        NoMatchingRuleFound = LLBT_ERR_NO_MATCHING_RULE_FOUND,
        InternalServerError = LLBT_ERR_INTERNAL_SERVER_ERROR,
        AuthProviderNotFound = LLBT_ERR_AUTH_PROVIDER_NOT_FOUND,
        AuthProviderAlreadyExists = LLBT_ERR_AUTH_PROVIDER_ALREADY_EXISTS,
        ServiceNotFound = LLBT_ERR_SERVICE_NOT_FOUND,
        ServiceTypeNotFound = LLBT_ERR_SERVICE_TYPE_NOT_FOUND,
        ServiceAlreadyExists = LLBT_ERR_SERVICE_ALREADY_EXISTS,
        ServiceCommandNotFound = LLBT_ERR_SERVICE_COMMAND_NOT_FOUND,
        ValueNotFound = LLBT_ERR_VALUE_NOT_FOUND,
        ValueAlreadyExists = LLBT_ERR_VALUE_ALREADY_EXISTS,
        ValueDuplicateName = LLBT_ERR_VALUE_DUPLICATE_NAME,
        FunctionNotFound = LLBT_ERR_FUNCTION_NOT_FOUND,
        FunctionAlreadyExists = LLBT_ERR_FUNCTION_ALREADY_EXISTS,
        FunctionDuplicateName = LLBT_ERR_FUNCTION_DUPLICATE_NAME,
        FunctionSyntaxError = LLBT_ERR_FUNCTION_SYNTAX_ERROR,
        FunctionInvalid = LLBT_ERR_FUNCTION_INVALID,
        IncomingWebhookNotFound = LLBT_ERR_INCOMING_WEBHOOK_NOT_FOUND,
        IncomingWebhookAlreadyExists = LLBT_ERR_INCOMING_WEBHOOK_ALREADY_EXISTS,
        IncomingWebhookDuplicateName = LLBT_ERR_INCOMING_WEBHOOK_DUPLICATE_NAME,
        RuleNotFound = LLBT_ERR_RULE_NOT_FOUND,
        APIKeyNotFound = LLBT_ERR_API_KEY_NOT_FOUND,
        RuleAlreadyExists = LLBT_ERR_RULE_ALREADY_EXISTS,
        RuleDuplicateName = LLBT_ERR_RULE_DUPLICATE_NAME,
        AuthProviderDuplicateName = LLBT_ERR_AUTH_PROVIDER_DUPLICATE_NAME,
        RestrictedHost = LLBT_ERR_RESTRICTED_HOST,
        APIKeyAlreadyExists = LLBT_ERR_API_KEY_ALREADY_EXISTS,
        IncomingWebhookAuthFailed = LLBT_ERR_INCOMING_WEBHOOK_AUTH_FAILED,
        ExecutionTimeLimitExceeded = LLBT_ERR_EXECUTION_TIME_LIMIT_EXCEEDED,
        NotCallable = LLBT_ERR_NOT_CALLABLE,
        UserAlreadyConfirmed = LLBT_ERR_USER_ALREADY_CONFIRMED,
        UserNotFound = LLBT_ERR_USER_NOT_FOUND,
        UserDisabled = LLBT_ERR_USER_DISABLED,
        AuthError = LLBT_ERR_AUTH_ERROR,
        BadRequest = LLBT_ERR_BAD_REQUEST,
        AccountNameInUse = LLBT_ERR_ACCOUNT_NAME_IN_USE,
        InvalidPassword = LLBT_ERR_INVALID_PASSWORD,
        SchemaValidationFailedWrite = LLBT_ERR_SCHEMA_VALIDATION_FAILED_WRITE,
        AppUnknownError = LLBT_ERR_APP_UNKNOWN,
        MaintenanceInProgress = LLBT_ERR_MAINTENANCE_IN_PROGRESS,
        UserpassTokenInvalid = LLBT_ERR_USERPASS_TOKEN_INVALID,
        InvalidServerResponse = LLBT_ERR_INVALID_SERVER_RESPONSE,
        AppServerError = LLBT_ERR_APP_SERVER_ERROR,

        CallbackFailed = LLBT_ERR_CALLBACK,
        UnknownError = LLBT_ERR_UNKNOWN,
    };

    static ErrorCategory error_categories(Error code);
    static std::string_view error_string(Error code);
    static Error from_string(std::string_view str);
    static std::vector<Error> get_all_codes();
    static std::vector<std::string_view> get_all_names();
    static std::vector<std::pair<std::string_view, ErrorCodes::Error>> get_error_list();
};

std::ostream& operator<<(std::ostream& stream, ErrorCodes::Error code);

} // namespace llbt
