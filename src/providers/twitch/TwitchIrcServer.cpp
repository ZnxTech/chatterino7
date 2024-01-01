#include "TwitchIrcServer.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/Env.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/bttv/BttvLiveUpdates.hpp"
#include "providers/seventv/eventapi/Subscription.hpp"
#include "providers/seventv/SeventvEventAPI.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/ChannelPointReward.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/PubSubManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/PostToThread.hpp"

#include <IrcCommand>
#include <QMetaEnum>

#include <cassert>

using namespace std::chrono_literals;

namespace {

const QString TWITCH_PUBSUB_URL = "wss://pubsub-edge.twitch.tv";
const QString BTTV_LIVE_UPDATES_URL = "wss://sockets.betterttv.net/ws";
const QString SEVENTV_EVENTAPI_URL = "wss://events.7tv.io/v3";

}  // namespace

namespace chatterino {

TwitchIrcServer::TwitchIrcServer()
    : whispersChannel(new Channel("/whispers", Channel::Type::TwitchWhispers))
    , mentionsChannel(new Channel("/mentions", Channel::Type::TwitchMentions))
    , liveChannel(new Channel("/live", Channel::Type::TwitchLive))
    , automodChannel(new Channel("/automod", Channel::Type::TwitchAutomod))
    , watchingChannel(Channel::getEmpty(), Channel::Type::TwitchWatching)
    , pubsub(new PubSub(TWITCH_PUBSUB_URL))
{
    this->initializeIrc();

    if (getSettings()->enableBTTVLiveUpdates &&
        getSettings()->enableBTTVChannelEmotes)
    {
        this->bttvLiveUpdates =
            std::make_unique<BttvLiveUpdates>(BTTV_LIVE_UPDATES_URL);
    }

    if (getSettings()->enableSevenTVEventAPI &&
        getSettings()->enableSevenTVChannelEmotes)
    {
        this->seventvEventAPI =
            std::make_unique<SeventvEventAPI>(SEVENTV_EVENTAPI_URL);
    }

    // getSettings()->twitchSeperateWriteConnection.connect([this](auto, auto) {
    // this->connect(); },
    //                                                     this->signalHolder_,
    //                                                     false);
}

void TwitchIrcServer::initialize(Settings &settings, Paths &paths)
{
    getApp()->accounts->twitch.currentUserChanged.connect([this]() {
        postToThread([this] {
            this->connect();
            this->pubsub->setAccount(getApp()->accounts->twitch.getCurrent());
        });
    });

    this->reloadBTTVGlobalEmotes();
    this->reloadFFZGlobalEmotes();
    this->reloadSevenTVGlobalEmotes();
}

void TwitchIrcServer::initializeConnection(IrcConnection *connection,
                                           ConnectionType type)
{
    std::shared_ptr<TwitchAccount> account =
        getApp()->accounts->twitch.getCurrent();

    qCDebug(chatterinoTwitch) << "logging in as" << account->getUserName();

    // twitch.tv/tags enables IRCv3 tags on messages. See https://dev.twitch.tv/docs/irc/tags
    // twitch.tv/commands enables a bunch of miscellaneous command capabilities. See https://dev.twitch.tv/docs/irc/commands
    // twitch.tv/membership enables the JOIN/PART/NAMES commands. See https://dev.twitch.tv/docs/irc/membership
    // This is enabled so we receive USERSTATE messages when joining channels / typing messages, along with the other command capabilities
    QStringList caps{"twitch.tv/tags", "twitch.tv/commands"};
    if (type != ConnectionType::Write)
    {
        caps.push_back("twitch.tv/membership");
    }

    connection->network()->setSkipCapabilityValidation(true);
    connection->network()->setRequestedCapabilities(caps);

    QString username = account->getUserName();
    QString oauthToken = account->getOAuthToken();

    if (!oauthToken.startsWith("oauth:"))
    {
        oauthToken.prepend("oauth:");
    }

    connection->setUserName(username);
    connection->setNickName(username);
    connection->setRealName(username);

    if (!account->isAnon())
    {
        connection->setPassword(oauthToken);
    }

    // https://dev.twitch.tv/docs/irc#connecting-to-the-twitch-irc-server
    // SSL disabled: irc://irc.chat.twitch.tv:6667 (or port 80)
    // SSL enabled: irc://irc.chat.twitch.tv:6697 (or port 443)
    connection->setHost(Env::get().twitchServerHost);
    connection->setPort(Env::get().twitchServerPort);
    connection->setSecure(Env::get().twitchServerSecure);

    this->open(type);
}

std::shared_ptr<Channel> TwitchIrcServer::createChannel(
    const QString &channelName)
{
    auto channel = std::make_shared<TwitchChannel>(channelName);
    channel->initialize();

    // We can safely ignore these signal connections since the TwitchIrcServer is only
    // ever destroyed when the full Application state is about to be destroyed, at which point
    // no Channel's should live
    // NOTE: CHANNEL_LIFETIME
    std::ignore = channel->sendMessageSignal.connect(
        [this, channel = channel.get()](auto &chan, auto &msg, bool &sent) {
            this->onMessageSendRequested(channel, msg, sent);
        });
    std::ignore = channel->sendReplySignal.connect(
        [this, channel = channel.get()](auto &chan, auto &msg, auto &replyId,
                                        bool &sent) {
            this->onReplySendRequested(channel, msg, replyId, sent);
        });

    return channel;
}

void TwitchIrcServer::privateMessageReceived(
    Communi::IrcPrivateMessage *message)
{
    IrcMessageHandler::instance().handlePrivMessage(message, *this);
}

void TwitchIrcServer::readConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    AbstractIrcServer::readConnectionMessageReceived(message);

    if (message->type() == Communi::IrcMessage::Type::Private)
    {
        // We already have a handler for private messages
        return;
    }

    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();

    // Below commands enabled through the twitch.tv/membership CAP REQ
    if (command == "JOIN")
    {
        handler.handleJoinMessage(message);
    }
    else if (command == "PART")
    {
        handler.handlePartMessage(message);
    }
    else if (command == "USERSTATE")
    {
        // Received USERSTATE upon JOINing a channel
        handler.handleUserStateMessage(message);
    }
    else if (command == "ROOMSTATE")
    {
        // Received ROOMSTATE upon JOINing a channel
        handler.handleRoomStateMessage(message);
    }
    else if (command == "CLEARCHAT")
    {
        handler.handleClearChatMessage(message);
    }
    else if (command == "CLEARMSG")
    {
        handler.handleClearMessageMessage(message);
    }
    else if (command == "USERNOTICE")
    {
        handler.handleUserNoticeMessage(message, *this);
    }
    else if (command == "NOTICE")
    {
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "WHISPER")
    {
        handler.handleWhisperMessage(message);
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->markChannelsConnected();
        this->connect();
    }
    else if (command == "GLOBALUSERSTATE")
    {
        handler.handleGlobalUserStateMessage(message);
    }
}

void TwitchIrcServer::writeConnectionMessageReceived(
    Communi::IrcMessage *message)
{
    const QString &command = message->command();

    auto &handler = IrcMessageHandler::instance();
    // Below commands enabled through the twitch.tv/commands CAP REQ
    if (command == "USERSTATE")
    {
        // Received USERSTATE upon sending PRIVMSG messages
        handler.handleUserStateMessage(message);
    }
    else if (command == "NOTICE")
    {
        // List of expected NOTICE messages on write connection
        // https://git.kotmisia.pl/Mm2PL/docs/src/branch/master/irc_msg_ids.md#command-results
        handler.handleNoticeMessage(
            static_cast<Communi::IrcNoticeMessage *>(message));
    }
    else if (command == "RECONNECT")
    {
        this->addGlobalSystemMessage(
            "Twitch Servers requested us to reconnect, reconnecting");
        this->connect();
    }
}

std::shared_ptr<Channel> TwitchIrcServer::getCustomChannel(
    const QString &channelName)
{
    if (channelName == "/whispers")
    {
        return this->whispersChannel;
    }

    if (channelName == "/mentions")
    {
        return this->mentionsChannel;
    }

    if (channelName == "/live")
    {
        return this->liveChannel;
    }

    if (channelName == "/automod")
    {
        return this->automodChannel;
    }

    static auto getTimer = [](ChannelPtr channel, int msBetweenMessages,
                              bool addInitialMessages) {
        if (addInitialMessages)
        {
            for (auto i = 0; i < 1000; i++)
            {
                channel->addMessage(makeSystemMessage(QString::number(i + 1)));
            }
        }

        auto *timer = new QTimer;
        QObject::connect(timer, &QTimer::timeout, [channel] {
            channel->addMessage(
                makeSystemMessage(QTime::currentTime().toString()));
        });
        timer->start(msBetweenMessages);
        return timer;
    };

    if (channelName == "$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, true);

        return channel;
    }
    if (channelName == "$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 500, false);

        return channel;
    }
    if (channelName == "$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, true);

        return channel;
    }
    if (channelName == "$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 250, false);

        return channel;
    }
    if (channelName == "$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, true);

        return channel;
    }
    if (channelName == "$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 100, false);

        return channel;
    }
    if (channelName == "$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, true);

        return channel;
    }
    if (channelName == "$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 50, false);

        return channel;
    }
    if (channelName == "$$$$$$$")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, true);

        return channel;
    }
    if (channelName == "$$$$$$$:e")
    {
        static auto channel = std::make_shared<Channel>(
            channelName, chatterino::Channel::Type::Misc);
        getTimer(channel, 25, false);

        return channel;
    }

    return nullptr;
}

void TwitchIrcServer::forEachChannelAndSpecialChannels(
    std::function<void(ChannelPtr)> func)
{
    this->forEachChannel(func);

    func(this->whispersChannel);
    func(this->mentionsChannel);
    func(this->liveChannel);
    func(this->automodChannel);
}

std::shared_ptr<Channel> TwitchIrcServer::getChannelOrEmptyByID(
    const QString &channelId)
{
    std::lock_guard<std::mutex> lock(this->channelMutex);

    for (const auto &weakChannel : this->channels)
    {
        auto channel = weakChannel.lock();
        if (!channel)
            continue;

        auto twitchChannel = std::dynamic_pointer_cast<TwitchChannel>(channel);
        if (!twitchChannel)
            continue;

        if (twitchChannel->roomId() == channelId &&
            twitchChannel->getName().count(':') < 2)
        {
            return twitchChannel;
        }
    }

    return Channel::getEmpty();
}

QString TwitchIrcServer::cleanChannelName(const QString &dirtyChannelName)
{
    if (dirtyChannelName.startsWith('#'))
        return dirtyChannelName.mid(1).toLower();
    else
        return dirtyChannelName.toLower();
}

bool TwitchIrcServer::hasSeparateWriteConnection() const
{
    return true;
    // return getSettings()->twitchSeperateWriteConnection;
}

bool TwitchIrcServer::prepareToSend(TwitchChannel *channel)
{
    std::lock_guard<std::mutex> guard(this->lastMessageMutex_);

    auto &lastMessage = channel->hasHighRateLimit() ? this->lastMessageMod_
                                                    : this->lastMessagePleb_;
    size_t maxMessageCount = channel->hasHighRateLimit() ? 99 : 19;
    auto minMessageOffset = (channel->hasHighRateLimit() ? 100ms : 1100ms);

    auto now = std::chrono::steady_clock::now();

    // check if you are sending messages too fast
    if (!lastMessage.empty() && lastMessage.back() + minMessageOffset > now)
    {
        if (this->lastErrorTimeSpeed_ + 30s < now)
        {
            auto errorMessage =
                makeSystemMessage("You are sending messages too quickly.");

            channel->addMessage(errorMessage);

            this->lastErrorTimeSpeed_ = now;
        }
        return false;
    }

    // remove messages older than 30 seconds
    while (!lastMessage.empty() && lastMessage.front() + 32s < now)
    {
        lastMessage.pop();
    }

    // check if you are sending too many messages
    if (lastMessage.size() >= maxMessageCount)
    {
        if (this->lastErrorTimeAmount_ + 30s < now)
        {
            auto errorMessage =
                makeSystemMessage("You are sending too many messages.");

            channel->addMessage(errorMessage);

            this->lastErrorTimeAmount_ = now;
        }
        return false;
    }

    lastMessage.push(now);
    return true;
}

void TwitchIrcServer::onMessageSendRequested(TwitchChannel *channel,
                                             const QString &message, bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    this->sendMessage(channel->getName(), message);
    sent = true;
}

void TwitchIrcServer::onReplySendRequested(TwitchChannel *channel,
                                           const QString &message,
                                           const QString &replyId, bool &sent)
{
    sent = false;

    bool canSend = this->prepareToSend(channel);
    if (!canSend)
    {
        return;
    }

    this->sendRawMessage("@reply-parent-msg-id=" + replyId + " PRIVMSG #" +
                         channel->getName() + " :" + message);

    sent = true;
}

const BttvEmotes &TwitchIrcServer::getBttvEmotes() const
{
    return this->bttv;
}
const FfzEmotes &TwitchIrcServer::getFfzEmotes() const
{
    return this->ffz;
}
const SeventvEmotes &TwitchIrcServer::getSeventvEmotes() const
{
    return this->seventv_;
}

const IndirectChannel &TwitchIrcServer::getWatchingChannel() const
{
    return this->watchingChannel;
}

void TwitchIrcServer::reloadBTTVGlobalEmotes()
{
    this->bttv.loadEmotes();
}

void TwitchIrcServer::reloadAllBTTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshBTTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadFFZGlobalEmotes()
{
    this->ffz.loadEmotes();
}

void TwitchIrcServer::reloadAllFFZChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshFFZChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::reloadSevenTVGlobalEmotes()
{
    this->seventv_.loadGlobalEmotes();
}

void TwitchIrcServer::reloadAllSevenTVChannelEmotes()
{
    this->forEachChannel([](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get()))
        {
            channel->refreshSevenTVChannelEmotes(false);
        }
    });
}

void TwitchIrcServer::forEachSeventvEmoteSet(
    const QString &emoteSetId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([emoteSetId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvEmoteSetID() == emoteSetId)
        {
            func(*channel);
        }
    });
}
void TwitchIrcServer::forEachSeventvUser(
    const QString &userId, std::function<void(TwitchChannel &)> func)
{
    this->forEachChannel([userId, func](const auto &chan) {
        if (auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
            channel->seventvUserID() == userId)
        {
            func(*channel);
        }
    });
}

void TwitchIrcServer::dropSeventvChannel(const QString &userID,
                                         const QString &emoteSetID)
{
    if (!this->seventvEventAPI)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(this->channelMutex);

    // ignore empty values
    bool skipUser = userID.isEmpty();
    bool skipSet = emoteSetID.isEmpty();

    bool foundUser = skipUser;
    bool foundSet = skipSet;
    for (std::weak_ptr<Channel> &weak : this->channels)
    {
        ChannelPtr chan = weak.lock();
        if (!chan)
        {
            continue;
        }

        auto *channel = dynamic_cast<TwitchChannel *>(chan.get());
        if (!foundSet && channel->seventvEmoteSetID() == emoteSetID)
        {
            foundSet = true;
        }
        if (!foundUser && channel->seventvUserID() == userID)
        {
            foundUser = true;
        }

        if (foundSet && foundUser)
        {
            break;
        }
    }

    if (!foundUser)
    {
        this->seventvEventAPI->unsubscribeUser(userID);
    }
    if (!foundSet)
    {
        this->seventvEventAPI->unsubscribeEmoteSet(emoteSetID);
    }
}

}  // namespace chatterino
