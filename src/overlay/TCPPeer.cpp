// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerManager.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/format.h"
#include "xdrpp/marshal.h"

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

const size_t TCPPeer::BUFSZ;

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<TCPPeer::SocketType> socket)
    : Peer(app, role), mSocket(socket)
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, PeerBareAddress const& address)
{
    assert(address.getType() == PeerBareAddress::Type::IPv4);

    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << " to " << address.toString();
    assertThreadIsMain();
    auto socket = make_shared<SocketType>(app.getClock().getIOContext(), BUFSZ);
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mAddress = address;
    result->startIdleTimer();
    asio::ip::tcp::endpoint endpoint(
        asio::ip::address::from_string(address.getIP()), address.getPort());
    socket->next_layer().async_connect(
        endpoint, [result](asio::error_code const& error) {
            asio::error_code ec;
            if (!error)
            {
                asio::ip::tcp::no_delay nodelay(true);
                result->mSocket->next_layer().set_option(nodelay, ec);
            }
            else
            {
                ec = error;
            }

            result->connectHandler(ec);
        });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<TCPPeer::SocketType> socket)
{
    assertThreadIsMain();
    shared_ptr<TCPPeer> result;
    asio::error_code ec;

    asio::ip::tcp::no_delay nodelay(true);
    socket->next_layer().set_option(nodelay, ec);

    if (!ec)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                               << "@" << app.getConfig().PEER_PORT;
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
        result->startIdleTimer();
        result->startRead();
    }
    else
    {
        CLOG(DEBUG, "Overlay")
            << "TCPPeer:accept"
            << "@" << app.getConfig().PEER_PORT << " error " << ec.message();
    }

    return result;
}

TCPPeer::~TCPPeer()
{
    assertThreadIsMain();
    mIdleTimer.cancel();
    if (mSocket)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
        asio::error_code ec;

#ifndef _WIN32
        // This always fails on windows and ASIO won't
        // even build it.
        mSocket->next_layer().cancel(ec);
#endif
        mSocket->close(ec);
    }
}

std::string
TCPPeer::getIP() const
{
    std::string result;

    asio::error_code ec;
    auto ep = mSocket->next_layer().remote_endpoint(ec);
    if (!ec)
    {
        result = ep.address().to_string();
    }

    return result;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    if (mState == CLOSING)
    {
        CLOG(ERROR, "Overlay")
            << "Trying to send message to " << toString() << " after drop";
        CLOG(ERROR, "Overlay") << REPORT_INTERNAL_BUG;
        return;
    }

    assertThreadIsMain();

    TimestampedMessage msg;
    msg.mEnqueuedTime = mApp.getClock().now();
    msg.mMessage = std::move(xdrBytes);
    mWriteQueue.emplace_back(std::move(msg));

    if (!mWriting)
    {
        mWriting = true;
        messageSender();
    }
}

void
TCPPeer::shutdown()
{
    if (mShutdownScheduled)
    {
        // should not happen, leave here for debugging purposes
        CLOG(ERROR, "Overlay") << "Double schedule of shutdown " << toString();
        CLOG(ERROR, "Overlay") << REPORT_INTERNAL_BUG;
        return;
    }

    mIdleTimer.cancel();
    mShutdownScheduled = true;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // To shutdown, we first queue up our desire to shutdown in the strand,
    // behind any pending read/write calls. We'll let them issue first.
    self->getApp().postOnMainThread(
        [self]() {
            // Gracefully shut down connection: this pushes a FIN packet into
            // TCP which, if we wanted to be really polite about, we would wait
            // for an ACK from by doing repeated reads until we get a 0-read.
            //
            // But since we _might_ be dropping a hostile or unresponsive
            // connection, we're going to just post a close() immediately after,
            // and hope the kernel does something useful as far as putting any
            // queued last-gasp ERROR_MSG packet on the wire.
            //
            // All of this is voluntary. We can also just close(2) here and be
            // done with it, but we want to give some chance of telling peers
            // why we're disconnecting them.
            asio::error_code ec;
            self->mSocket->next_layer().shutdown(
                asio::ip::tcp::socket::shutdown_both, ec);
            if (ec)
            {
                CLOG(DEBUG, "Overlay")
                    << "TCPPeer::drop shutdown socket failed: " << ec.message();
            }
            self->getApp().postOnMainThread(
                [self]() {
                    // Close fd associated with socket. Socket is already shut
                    // down, but depending on platform (and apparently whether
                    // there was unread data when we issued shutdown()) this
                    // call might push RST onto the wire, or some other action;
                    // in any case it has to be done to free the OS resources.
                    //
                    // It will also, at this point, cancel any pending asio
                    // read/write handlers, i.e. fire them with an error code
                    // indicating cancellation.
                    asio::error_code ec2;
                    self->mSocket->close(ec2);
                    if (ec2)
                    {
                        CLOG(DEBUG, "Overlay")
                            << "TCPPeer::drop close socket failed: "
                            << ec2.message();
                    }
                },
                "TCPPeer: close");
        },
        "TCPPeer: shutdown");
}

void
TCPPeer::messageSender()
{
    assertThreadIsMain();

    // if nothing to do, mark progress and return.
    if (mWriteQueue.empty())
    {
        mWriting = false;
        // there is nothing to send and delayed shutdown was
        // requested - time to perform it
        if (mDelayedShutdown)
        {
            shutdown();
        }
        return;
    }

    // Take a snapshot of the contents of mWriteQueue into mWriteBuffers, in
    // terms of asio::const_buffers pointing into the elements of mWriteQueue,
    // and then issue a single multi-buffer ("scatter-gather") async_write that
    // covers the whole snapshot. We'll get called back when the batch is
    // completed, at which point we'll clear mWriteBuffers and remove the entire
    // snapshot worth of corresponding messages from mWriteQueue (though it may
    // have grown a bit in the meantime -- we remove only a prefix).
    assert(mWriteBuffers.empty());
    auto now = mApp.getClock().now();
    size_t expected_length = 0;
    size_t maxQueueSize = mApp.getConfig().MAX_BATCH_WRITE_COUNT;
    assert(maxQueueSize > 0);
    size_t const maxTotalBytes = mApp.getConfig().MAX_BATCH_WRITE_BYTES;
    for (auto& tsm : mWriteQueue)
    {
        tsm.mIssuedTime = now;
        size_t sz = tsm.mMessage->raw_size();
        mWriteBuffers.emplace_back(tsm.mMessage->raw_data(), sz);
        expected_length += sz;
        mEnqueueTimeOfLastWrite = tsm.mEnqueuedTime;
        // check if we reached any limit
        if (expected_length >= maxTotalBytes)
            break;
        if (--maxQueueSize == 0)
            break;
    }

    if (Logging::logDebug("Overlay"))
    {
        CLOG(DEBUG, "Overlay") << fmt::format(
            "messageSender {} - b:{} n:{}/{}", toString(), expected_length,
            mWriteBuffers.size(), mWriteQueue.size());
    }
    getOverlayMetrics().mAsyncWrite.Mark();
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    asio::async_write(*(mSocket.get()), mWriteBuffers,
                      [self, expected_length](asio::error_code const& ec,
                                              std::size_t length) {
                          if (expected_length != length)
                          {
                              self->drop("error during async_write",
                                         Peer::DropDirection::WE_DROPPED_REMOTE,
                                         Peer::DropMode::IGNORE_WRITE_QUEUE);
                              return;
                          }
                          self->writeHandler(ec, length,
                                             self->mWriteBuffers.size());

                          // Walk through a _prefix_ of the write queue
                          // _corresponding_ to the write buffers we just sent.
                          // While walking, record the sent-time in metrics, but
                          // also advance iterator 'i' so we wind up with an
                          // iterator range to erase from the front of the write
                          // queue.
                          auto now = self->mApp.getClock().now();
                          auto i = self->mWriteQueue.begin();
                          while (!self->mWriteBuffers.empty())
                          {
                              i->mCompletedTime = now;
                              i->recordWriteTiming(self->getOverlayMetrics());
                              ++i;
                              self->mWriteBuffers.pop_back();
                          }

                          // Erase the messages from the write queue that we
                          // just forgot about the buffers for.
                          self->mWriteQueue.erase(self->mWriteQueue.begin(), i);

                          // continue processing the queue
                          if (!ec)
                          {
                              self->messageSender();
                          }
                      });
}

void
TCPPeer::TimestampedMessage::recordWriteTiming(OverlayMetrics& metrics)
{
    auto qdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mIssuedTime - mEnqueuedTime);
    auto wdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mCompletedTime - mIssuedTime);
    metrics.mMessageDelayInWriteQueueTimer.Update(qdelay);
    metrics.mMessageDelayInAsyncWriteTimer.Update(wdelay);
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred,
                      size_t messages_transferred)
{
    assertThreadIsMain();
    mLastWrite = mApp.getClock().now();

    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            getOverlayMetrics().mErrorWrite.Mark();
            CLOG(ERROR, "Overlay")
                << "Error during sending message to " << toString();
        }
        if (mDelayedShutdown)
        {
            // delayed shutdown was requested - time to perform it
            shutdown();
        }
        else
        {
            // no delayed shutdown - we can drop normally
            drop("error during write", Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
    }
    else if (bytes_transferred != 0)
    {
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        getOverlayMetrics().mMessageWrite.Mark(messages_transferred);
        getOverlayMetrics().mByteWrite.Mark(bytes_transferred);

        mPeerMetrics.mMessageWrite += messages_transferred;
        mPeerMetrics.mByteWrite += bytes_transferred;
    }
}

void
TCPPeer::noteErrorReadHeader(size_t nbytes, asio::error_code const& ec)
{
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    std::string msg("error reading message header: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteShortReadHeader(size_t nbytes)
{
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    drop("short read of message header", Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteFullyReadHeader()
{
    receivedBytes(HDRSZ, false);
}

void
TCPPeer::noteErrorReadBody(size_t nbytes, asio::error_code const& ec)
{
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    std::string msg("error reading message body: ");
    msg.append(ec.message());
    drop(msg, Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteShortReadBody(size_t nbytes)
{
    receivedBytes(nbytes, false);
    getOverlayMetrics().mErrorRead.Mark();
    drop("short read of message body", Peer::DropDirection::WE_DROPPED_REMOTE,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
TCPPeer::noteFullyReadBody(size_t nbytes)
{
    receivedBytes(nbytes, true);
}

void
TCPPeer::startRead()
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    mIncomingHeader.clear();

    CLOG(DEBUG, "Overlay") << "TCPPeer::startRead " << mSocket->in_avail()
                           << " from " << toString();

    mIncomingHeader.resize(HDRSZ);

    // We read large-ish (256KB) buffers of data from TCP which might have quite
    // a few messages in them. We want to digest as many of these
    // _synchronously_ as we can before we issue an async_read against ASIO.
    YieldTimer yt(mApp.getClock(), mApp.getConfig().MAX_BATCH_READ_PERIOD_MS,
                  mApp.getConfig().MAX_BATCH_READ_COUNT);
    while (mSocket->in_avail() >= HDRSZ && yt.shouldKeepGoing())
    {
        asio::error_code ec_hdr, ec_body;
        size_t n = mSocket->read_some(asio::buffer(mIncomingHeader), ec_hdr);
        if (ec_hdr)
        {
            noteErrorReadHeader(n, ec_hdr);
            return;
        }
        if (n != HDRSZ)
        {
            noteShortReadHeader(n);
            return;
        }
        size_t length = getIncomingMsgLength();
        if (mSocket->in_avail() >= length)
        {
            // We can finish reading a full message here synchronously,
            // which means we will count the received header bytes here.
            noteFullyReadHeader();
            if (length != 0)
            {
                mIncomingBody.resize(length);
                n = mSocket->read_some(asio::buffer(mIncomingBody), ec_body);
                if (ec_body)
                {
                    noteErrorReadBody(n, ec_body);
                    return;
                }
                if (n != length)
                {
                    noteShortReadBody(n);
                    return;
                }
                noteFullyReadBody(length);
                recvMessage();
            }
        }
        else
        {
            // We read a header synchronously, but don't have enough data in the
            // buffered_stream to read the body synchronously. Pretend we just
            // finished reading the header asynchronously, and punt to
            // readHeaderHandler to let it re-read the header and issue an async
            // read for the body.
            readHeaderHandler(asio::error_code(), HDRSZ);
            return;
        }
    }

    if (mSocket->in_avail() < HDRSZ)
    {
        // If there wasn't enough readable in the buffered stream to even get a
        // header (message length), issue an async_read and hope that the
        // buffering pulls in much more than just the 4 bytes we ask for here.
        getOverlayMetrics().mAsyncRead.Mark();
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());
        asio::async_read(*(mSocket.get()), asio::buffer(mIncomingHeader),
                         [self](asio::error_code ec, std::size_t length) {
                             self->readHeaderHandler(ec, length);
                         });
    }
    else
    {
        // we have enough data but need to bounce on the main thread as we've
        // done too much work already
        auto self = static_pointer_cast<TCPPeer>(shared_from_this());
        self->getApp().postOnMainThread(
            [self]() { self->startRead(); },
            fmt::format("{} TCPPeer: startRead", toString()));
    }
}

size_t
TCPPeer::getIncomingMsgLength()
{
    size_t length = static_cast<size_t>(mIncomingHeader[0]);
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    if (length <= 0 ||
        (!isAuthenticated() && (length > MAX_UNAUTH_MESSAGE_SIZE)) ||
        length > MAX_MESSAGE_SIZE)
    {
        getOverlayMetrics().mErrorRead.Mark();
        CLOG(ERROR, "Overlay")
            << "TCP: message size unacceptable: " << length
            << (isAuthenticated() ? "" : " while not authenticated");
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        length = 0;
    }
    return (length);
}

void
TCPPeer::connected()
{
    startRead();
}

void
TCPPeer::readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred)
{
    assertThreadIsMain();

    if (error)
    {
        noteErrorReadHeader(bytes_transferred, error);
    }
    else if (bytes_transferred != HDRSZ)
    {
        noteShortReadHeader(bytes_transferred);
    }
    else
    {
        noteFullyReadHeader();
        size_t expected_length = getIncomingMsgLength();
        if (expected_length != 0)
        {
            mIncomingBody.resize(expected_length);
            auto self = static_pointer_cast<TCPPeer>(shared_from_this());
            asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [self, expected_length](asio::error_code ec,
                                                     std::size_t length) {
                                 self->readBodyHandler(ec, length,
                                                       expected_length);
                             });
        }
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred,
                         std::size_t expected_length)
{
    assertThreadIsMain();

    if (error)
    {
        noteErrorReadBody(bytes_transferred, error);
    }
    else if (bytes_transferred != expected_length)
    {
        noteShortReadBody(bytes_transferred);
    }
    else
    {
        noteFullyReadBody(bytes_transferred);
        recvMessage();
        mIncomingHeader.clear();
        startRead();
    }
}

void
TCPPeer::recvMessage()
{
    assertThreadIsMain();

    try
    {
        xdr::xdr_get g(mIncomingBody.data(),
                       mIncomingBody.data() + mIncomingBody.size());
        AuthenticatedMessage am;
        xdr::xdr_argpack_archive(g, am);

        Peer::recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "recvMessage got a corrupt xdr: " << e.what();
        sendErrorAndDrop(ERR_DATA, "received corrupt XDR",
                         Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

void
TCPPeer::drop(std::string const& reason, DropDirection dropDirection,
              DropMode dropMode)
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    if (mState != GOT_AUTH)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer::drop " << toString() << " in state "
                               << mState << " we called:" << mRole;
    }
    else if (dropDirection == Peer::DropDirection::WE_DROPPED_REMOTE)
    {
        CLOG(INFO, "Overlay")
            << "Dropping peer " << toString() << "; reason: " << reason;
    }
    else
    {
        CLOG(INFO, "Overlay")
            << "Peer " << toString() << " dropped us; reason: " << reason;
    }

    mState = CLOSING;

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    getApp().getOverlayManager().removePeer(this);

    // if write queue is not empty, messageSender will take care of shutdown
    if ((dropMode == Peer::DropMode::IGNORE_WRITE_QUEUE) || !mWriting)
    {
        self->shutdown();
    }
    else
    {
        self->mDelayedShutdown = true;
    }
}
}
