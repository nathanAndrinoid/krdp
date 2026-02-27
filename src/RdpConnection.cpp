// SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
//
// This file is roughly based on grd-session-rdp.c from gnome-remote-desktop,
// which is:
//
// SPDX-FileCopyrightText: 2020-2023 Pascal Nowack
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "RdpConnection.h"

#include <algorithm>
#include <filesystem>
#include <optional>

#include <fcntl.h>

#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QThread>
#include <QtGlobal>

#include <freerdp/channels/wtsvc.h>
#include <freerdp/freerdp.h>
#include <freerdp/server/cliprdr.h>

#include <freerdp/channels/drdynvc.h>

#include "Clipboard.h"
#include "Cursor.h"
#include "InputHandler.h"
#include "NetworkDetection.h"
#include "PeerContext_p.h"
#include "Server.h"
#include "VideoStream.h"

#include <KUser>

#include "krdp_logging.h"

namespace fs = std::filesystem;

namespace KRdp
{

#include <security/pam_appl.h>

typedef struct {
    QByteArray user;
    QByteArray password;
} RDPConnectionAuthData;

typedef struct {
    pam_handle_t *handle;
    struct pam_conv pamc;
    RDPConnectionAuthData appdata;
} RdpConnectionPamHandle;

static int pam_conv(int num_msg, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr)
{
    int pam_status = PAM_CONV_ERR;
    RDPConnectionAuthData *appdata = NULL;
    struct pam_response *response = NULL;
    WINPR_ASSERT(num_msg >= 0);
    appdata = (RDPConnectionAuthData *)appdata_ptr;
    WINPR_ASSERT(appdata);

    if (!(response = (struct pam_response *)calloc((size_t)num_msg, sizeof(struct pam_response))))
        return PAM_BUF_ERR;

    for (int index = 0; index < num_msg; index++) {
        switch (msg[index]->msg_style) {
        case PAM_PROMPT_ECHO_ON:
            response[index].resp = _strdup(appdata->user.constData());

            if (!response[index].resp)
                goto out_fail;

            response[index].resp_retcode = PAM_SUCCESS;
            break;

        case PAM_PROMPT_ECHO_OFF:
            response[index].resp = _strdup(appdata->password.constData());

            if (!response[index].resp)
                goto out_fail;

            response[index].resp_retcode = PAM_SUCCESS;
            break;

        default:
            pam_status = PAM_CONV_ERR;
            goto out_fail;
        }
    }

    *resp = response;
    return PAM_SUCCESS;
out_fail:

    for (int index = 0; index < num_msg; ++index) {
        if (response[index].resp) {
            memset(response[index].resp, 0, strlen(response[index].resp));
            free(response[index].resp);
        }
    }

    memset(response, 0, sizeof(struct pam_response) * (size_t)num_msg);
    free(response);
    *resp = NULL;
    return pam_status;
}

static int pamAuthenticate(const QString &user, const QString &password)
{
    int pam_status = 0;
    RdpConnectionPamHandle info = {0};

    info.appdata.user = user.toLatin1();
    info.appdata.password = password.toLatin1();
    info.pamc.conv = &pam_conv;
    info.pamc.appdata_ptr = &info.appdata;

    pam_status = pam_start("login", 0, &info.pamc, &info.handle);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_start failure:" << pam_strerror(info.handle, pam_status);
        return -1;
    }

    pam_status = pam_authenticate(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_authenticate failure:" << pam_strerror(info.handle, pam_status);
        return -1;
    }

    pam_status = pam_acct_mgmt(info.handle, 0);

    if (pam_status != PAM_SUCCESS) {
        qWarning() << "pam_acct_mgmt failure:" << pam_strerror(info.handle, pam_status);
        return -1;
    }

    return 1;
}

static QString normalizeLoginName(QString userName)
{
    userName = userName.trimmed();

    // Some clients send DOMAIN\\user or DOMAIN/user.
    const int separatorIndex = std::max(userName.lastIndexOf(u'\\'), userName.lastIndexOf(u'/'));
    if (separatorIndex >= 0 && separatorIndex + 1 < userName.size()) {
        userName = userName.mid(separatorIndex + 1);
    }

    return userName.trimmed();
}

static bool matchesLoginName(const QString &actualName, const QString &configuredName)
{
    if (configuredName.isEmpty()) {
        return false;
    }

    if (actualName.compare(configuredName, Qt::CaseInsensitive) == 0) {
        return true;
    }

    // Some clients send user@domain. Accept when the local part matches.
    const int atSignIndex = actualName.indexOf(u'@');
    if (atSignIndex > 0) {
        const QStringView localPart = QStringView(actualName).left(atSignIndex);
        if (localPart.compare(configuredName, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

static QString authIdentityString(const UINT16 *value, ULONG length)
{
    if (!value || length == 0) {
        return {};
    }

    return QString::fromUtf16(reinterpret_cast<const char16_t *>(value), static_cast<qsizetype>(length));
}

static bool nlaEnabledFromEnvironment()
{
    if (!qEnvironmentVariableIsSet("KRDP_ENABLE_NLA")) {
        return false;
    }

    const QString value = QString::fromLatin1(qgetenv("KRDP_ENABLE_NLA")).trimmed().toLower();
    return value != QLatin1String("0")
        && value != QLatin1String("false")
        && value != QLatin1String("no")
        && value != QLatin1String("off");
}

/**
 * FreeRDP callback for the capabilities event.
 */
BOOL peerCapabilities(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onCapabilities()) {
        return TRUE;
    }

    return FALSE;
}

/**
 * FreeRDP callback for the post connect event.
 */
BOOL peerPostConnect(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onPostConnect()) {
        return TRUE;
    }

    return FALSE;
}

/**
 * FreeRDP callback for the activate event.
 */
BOOL peerActivate(freerdp_peer *peer)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onActivate()) {
        return TRUE;
    }

    return FALSE;
}

BOOL peerLogon(freerdp_peer *peer, const SEC_WINNT_AUTH_IDENTITY *identity, BOOL automatic)
{
    auto context = reinterpret_cast<PeerContext *>(peer->context);
    if (context->connection->onLogon(identity, automatic)) {
        return TRUE;
    }

    return FALSE;
}

BOOL suppressOutput(rdpContext *context, uint8_t allow, const RECTANGLE_16 *)
{
    auto peerContext = reinterpret_cast<PeerContext *>(context);
    if (peerContext->connection->onSuppressOutput(allow)) {
        return TRUE;
    }

    return FALSE;
}

class KRDP_NO_EXPORT RdpConnection::Private
{
public:
    Server *server = nullptr;

    State state = State::Initial;

    qintptr socketHandle;

    std::unique_ptr<InputHandler> inputHandler;
    std::unique_ptr<VideoStream> videoStream;
    std::unique_ptr<Cursor> cursor;
    std::unique_ptr<NetworkDetection> networkDetection;
    std::unique_ptr<Clipboard> clipboard;

    freerdp_peer *peer = nullptr;
    bool logonDecisionPresent = false;
    bool logonAccepted = false;

    std::jthread thread;
};

RdpConnection::RdpConnection(Server *server, qintptr socketHandle)
    : QObject(nullptr)
    , d(std::make_unique<Private>())
{
    d->server = server;
    d->socketHandle = socketHandle;

    d->inputHandler = std::make_unique<InputHandler>(this);
    d->videoStream = std::make_unique<VideoStream>(this);
    connect(d->videoStream.get(), &VideoStream::closed, this, [this]() {
        if (d->state == State::Running || d->state == State::Streaming) {
            qCDebug(KRDP) << "Video stream closed, closing session";
            d->peer->Close(d->peer);
        }
    });
    d->cursor = std::make_unique<Cursor>(this);
    d->networkDetection = std::make_unique<NetworkDetection>(this);
    d->clipboard = std::make_unique<Clipboard>(this);

    QMetaObject::invokeMethod(this, &RdpConnection::initialize, Qt::QueuedConnection);
}

RdpConnection::~RdpConnection()
{
    if (d->state == State::Streaming) {
        d->peer->Close(d->peer);
    }

    if (d->thread.joinable()) {
        d->thread.request_stop();
        d->thread.join();
    }

    if (d->peer) {
        freerdp_peer_free(d->peer);
    }
}

RdpConnection::State RdpConnection::state() const
{
    return d->state;
}

void RdpConnection::setState(KRdp::RdpConnection::State newState)
{
    if (newState == d->state) {
        return;
    }

    d->state = newState;
    Q_EMIT stateChanged(newState);
}

void RdpConnection::close(RdpConnection::CloseReason reason)
{
    switch (reason) {
    case CloseReason::VideoInitFailed:
        freerdp_set_error_info(d->peer->context->rdp, ERRINFO_GRAPHICS_SUBSYSTEM_FAILED);
        break;
    case CloseReason::None:
        break;
    }

    if (d->peer) { // may be null if creating the peer failed
        d->peer->Close(d->peer);
    }
}

InputHandler *RdpConnection::inputHandler() const
{
    return d->inputHandler.get();
}

KRdp::VideoStream *RdpConnection::videoStream() const
{
    return d->videoStream.get();
}

Cursor *RdpConnection::cursor() const
{
    return d->cursor.get();
}

Clipboard *RdpConnection::clipboard() const
{
    return d->clipboard.get();
}

NetworkDetection *RdpConnection::networkDetection() const
{
    return d->networkDetection.get();
}

void RdpConnection::initialize()
{
    setState(State::Starting);

    d->peer = freerdp_peer_new(d->socketHandle);
    if (!d->peer) {
        qCWarning(KRDP) << "Failed to create peer";
        return;
    }

    // Create an instance of our custom PeerContext extended context as context
    // rather than the plain rdpContext.
    d->peer->ContextSize = sizeof(PeerContext);
    d->peer->ContextNew = (psPeerContextNew)newPeerContext;
    d->peer->ContextFree = (psPeerContextFree)freePeerContext;

    auto result = freerdp_peer_context_new_ex(d->peer, d->server->rdpSettings());
    if (!result) {
        qCWarning(KRDP) << "Failed to create peer context";
        return;
    }

    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    context->connection = this;

    auto settings = d->peer->context->settings;

    auto certificate = freerdp_certificate_new_from_file(d->server->tlsCertificate().string().data());
    if (!certificate) {
        qCWarning(KRDP) << "Could not read certificate file" << d->server->tlsCertificate().string();
        return;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, certificate, 1);

    auto key = freerdp_key_new_from_file(d->server->tlsCertificateKey().string().data());
    if (!key) {
        qCWarning(KRDP) << "Could not read certificate file" << d->server->tlsCertificate().string();
        return;
    }
    freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);

    freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, false);
    freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, true);
    // NLA via WinPR SAM can fail on Linux and add a failing pre-auth roundtrip.
    // Keep it opt-in for clients that explicitly require it.
    const bool enableNla = nlaEnabledFromEnvironment();
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, enableNla);
    qCDebug(KRDP) << "NLA security enabled:" << enableNla;

    freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    // PSEUDO_XSERVER is apparently required for things to work properly.
    freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);

    // TODO: Implement audio support
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, false);

    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);

    // Plain YUV420 AVC is currently the most straightforward of the the AVC
    // related codecs to implement. Moreover, it makes the encoding side also
    // simpler so it is currently the only supported codec. This uses the RdpGfx
    // pipeline, so make sure to request that.
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, true);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444, false);
    freerdp_settings_set_bool(settings, FreeRDP_GfxAVC444v2, false);
    freerdp_settings_set_bool(settings, FreeRDP_GfxH264, true);


    freerdp_settings_set_bool(settings, FreeRDP_GfxSmallCache, false);
    freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, false);

    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, true);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, true);
    freerdp_settings_set_bool(settings, FreeRDP_UnicodeInput, true);

    // TODO: Implement network performance detection
    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, true);

    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, true);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteConsoleAudio, true);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, false);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, false);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, true);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, true);

    d->peer->Capabilities = peerCapabilities;
    d->peer->Activate = peerActivate;
    d->peer->Logon = peerLogon;
    d->peer->PostConnect = peerPostConnect;

    d->peer->context->update->SuppressOutput = suppressOutput;

    d->inputHandler->initialize(d->peer->context->input);
    context->inputHandler = d->inputHandler.get();

    context->networkDetection = d->networkDetection.get();
    d->networkDetection->initialize();

    if (!d->peer->Initialize(d->peer)) {
        qCWarning(KRDP) << "Unable to initialize peer";
        return;
    }

    qCDebug(KRDP) << "Session setup completed, start processing...";

    // Perform actual communication on a separate thread.
    d->thread = std::jthread(std::bind(&RdpConnection::run, this, std::placeholders::_1));
    pthread_setname_np(d->thread.native_handle(), "krdp_session");
}

void RdpConnection::run(std::stop_token stopToken)
{
    auto context = reinterpret_cast<PeerContext *>(d->peer->context);
    auto channelEvent = WTSVirtualChannelManagerGetEventHandle(context->virtualChannelManager);

    setState(State::Running);

    while (!stopToken.stop_requested()) {
        std::array<HANDLE, 32> events{channelEvent};
        auto handleCount = d->peer->GetEventHandles(d->peer, events.data() + 1, 31);
        if (handleCount <= 0) {
            qCDebug(KRDP) << "Unable to get transport event handles";
            break;
        }
        // Wait for something to happen on the connection.
        WaitForMultipleObjects(1 + handleCount, events.data(), FALSE, INFINITE);

        // Read data from the socket and have FreeRDP process it.
        if (d->peer->CheckFileDescriptor(d->peer) != TRUE) {
            qCDebug(KRDP) << "Unable to check file descriptor";
            break;
        }

        // Initialize any dynamic channels once the dynamic channel channel is setup.
        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, DRDYNVC_SVC_CHANNEL_NAME)) {
            auto state = WTSVirtualChannelManagerGetDrdynvcState(context->virtualChannelManager);
            // Dynamic channels can only be set up properly once the dynamic channel channel is properly setup.
            if (state == DRDYNVC_STATE_READY) {
                if (d->videoStream->initialize()) {
                    d->videoStream->setEnabled(true);
                    setState(State::Streaming);
                } else {
                    break;
                }
            } else if (state == DRDYNVC_STATE_NONE) {
                // This ensures that WTSVirtualChannelManagerCheckFileDescriptor() will be called, which initializes the drdynvc channel.
                SetEvent(channelEvent);
            }
        }

        if (WaitForSingleObject(channelEvent, 0) == WAIT_OBJECT_0 && WTSVirtualChannelManagerCheckFileDescriptor(context->virtualChannelManager) != TRUE) {
            qCDebug(KRDP) << "Unable to check Virtual Channel Manager file descriptor, closing connection";
            break;
        }

        if (d->peer->connected && WTSVirtualChannelManagerIsChannelJoined(context->virtualChannelManager, CLIPRDR_SVC_CHANNEL_NAME)) {
            if (!d->clipboard->initialize()) {
                break;
            }
        }

        d->networkDetection->update();
    }

    qCDebug(KRDP) << "Closing session";
    onClose();
}

bool RdpConnection::onCapabilities()
{
    auto settings = d->peer->context->settings;
    // We only support GraphicsPipeline clients currently as that is required
    // for AVC streaming.
    if (!freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline)) {
        qCWarning(KRDP) << "Client does not support graphics pipeline which is required";
        return false;
    }

    auto colorDepth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
    if (colorDepth != 32) {
        qCDebug(KRDP) << "Correcting invalid color depth from client:" << colorDepth;
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    }

    if (!freerdp_settings_get_bool(settings, FreeRDP_DesktopResize)) {
        qCWarning(KRDP) << "Client doesn't support resizing, aborting";
        return false;
    }

    if (freerdp_settings_get_uint32(settings,FreeRDP_PointerCacheSize) <= 0) {
        qCWarning(KRDP) << "Client doesn't support pointer caching, aborting";
        return false;
    }

    return true;
}

bool RdpConnection::onActivate()
{
    return true;
}

bool RdpConnection::onLogon(const SEC_WINNT_AUTH_IDENTITY *identity, BOOL automatic)
{
    QString username;
    QString password;

    if (identity) {
        const QString user = authIdentityString(identity->User, identity->UserLength);
        const QString domain = authIdentityString(identity->Domain, identity->DomainLength);
        password = authIdentityString(identity->Password, identity->PasswordLength);
        username = domain.isEmpty() ? user : QStringLiteral("%1\\%2").arg(domain, user);

        qCDebug(KRDP) << "Logon callback user:" << username << "automatic:" << (automatic == TRUE) << "passwordLength:" << password.size();
    } else {
        qCWarning(KRDP) << "Logon callback did not provide an identity";
    }

    // Some clients do not provide credentials here and only send them later.
    // Defer to PostConnect in that case.
    if (username.isEmpty() && password.isEmpty()) {
        qCDebug(KRDP) << "Logon callback had no credentials; deferring authentication to PostConnect";
        d->logonDecisionPresent = false;
        d->logonAccepted = false;
        return true;
    }

    d->logonAccepted = authenticateLogin(username, password);
    d->logonDecisionPresent = true;
    return d->logonAccepted;
}

bool RdpConnection::authenticateLogin(const QString &rawUsername, const QString &password)
{
    const QString username = normalizeLoginName(rawUsername);

    qCDebug(KRDP) << "Authenticating RDP login for user" << rawUsername << "(normalized to" << username << ", passwordLength:" << password.size() << ")";

    if (d->server->usePAMAuthentication()) {
        qCDebug(KRDP) << "Attempting authenticating user with PAM";
        if (matchesLoginName(username, KUser().loginName()) && pamAuthenticate(KUser().loginName(), password) >= 0) {
            qCDebug(KRDP) << "PAM authentication succeeded for user" << username;
            return true;
        }
    }

    const auto users = d->server->users();
    for (const auto &user : users) {
        if (user.password.isEmpty()) {
            qCWarning(KRDP) << "Skipping configured RDP user with empty password:" << user.name;
            continue;
        }
        if (matchesLoginName(username, user.name) && user.password == password) {
            qCDebug(KRDP) << "User" << username << "authenticated successfully";
            return true;
        }
    }

    qCWarning(KRDP) << "Authentication failed for user" << rawUsername;
    return false;
}

bool RdpConnection::onPostConnect()
{
    qCInfo(KRDP) << "New client connected:" << d->peer->hostname << freerdp_peer_os_major_type_string(d->peer) << freerdp_peer_os_minor_type_string(d->peer);

    rdpSettings *settings = d->peer->context->settings;

    if (!freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, true)) {
        return false;
    }

    if (d->logonDecisionPresent) {
        qCDebug(KRDP) << "Using authentication result from Logon callback:" << d->logonAccepted;
        return d->logonAccepted;
    }

    QString rawUsername = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Username));
    QString password = QString::fromLatin1(freerdp_settings_get_string(settings, FreeRDP_Password));

    // Some clients only expose credentials through the peer identity.
    if ((rawUsername.isEmpty() || password.isEmpty()) && d->peer->identity.User) {
        const QString user = authIdentityString(d->peer->identity.User, d->peer->identity.UserLength);
        const QString domain = authIdentityString(d->peer->identity.Domain, d->peer->identity.DomainLength);
        const QString identityPassword = authIdentityString(d->peer->identity.Password, d->peer->identity.PasswordLength);

        if (rawUsername.isEmpty()) {
            rawUsername = domain.isEmpty() ? user : QStringLiteral("%1\\%2").arg(domain, user);
        }
        if (password.isEmpty()) {
            password = identityPassword;
        }
    }

    return authenticateLogin(rawUsername, password);
}

bool RdpConnection::onClose()
{
    d->clipboard->close();
    d->videoStream->close();
    setState(State::Closed);
    return true;
}

bool RdpConnection::onSuppressOutput(uint8_t allow)
{
    if (allow) {
        d->videoStream->setEnabled(true);
    } else {
        d->videoStream->setEnabled(false);
    }

    return true;
}

freerdp_peer *RdpConnection::rdpPeer() const
{
    return d->peer;
}

rdpContext *RdpConnection::rdpPeerContext() const
{
    return d->peer->context;
}
}

#include "moc_RdpConnection.cpp"
