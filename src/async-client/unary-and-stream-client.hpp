#pragma once

#include <array>
#include <functional>
#include <mutex>

#include <boost/type_index.hpp>
#include <boost/type_index/runtime_cast/register_runtime_class.hpp>

#include <grpcpp/grpcpp.h>

#include "route_guide.grpc.pb.h"
#include "funwithgrpc/logging.h"
#include "Config.h"


class UnaryAndSingleStreamClient {
public:

    /*! Base class for requests
     *
     *  In order to use `this` as a tag and avoid any special processing in the
     *  event-loop, the simplest approacch in C++ is to let the request implementations
     *  inherit form a base-class that contains the shared code they all need, and
     *  a pure virtual method for the state-machine.
     */
    class RequestBase {
    public:

        /*! Tag
         *
         *  In order to allow tags for multiple async operations simultaneously,
         *  we use this "Handle". It points to the request owning the
         *  operation, and it is associated with a type of operation.
         */
        class Handle {
        public:
            enum Operation {
                CONNECT,
                READ,
                WRITE,
                FINISH
            };

            Handle(RequestBase& instance, Operation op)
                : instance_{instance}, op_{op} {}

            void *tag() {
                return this;
            }

            void proceed(bool ok) {
                return instance_.proceed(ok, op_);
            }

        private:
            RequestBase& instance_;
            const Operation op_;
        };

        RequestBase(UnaryAndSingleStreamClient& parent)
            : parent_{parent} {}

        virtual ~RequestBase() = default;

        // The state-machine
        virtual void proceed(bool ok, Handle::Operation op) = 0;


        void done() {
            // Ugly, ugly, ugly
            LOG_TRACE << "If the program crash now, it was a bad idea to delete this ;)";

            // Reference-counting of instances of requests in flight
            parent_.decCounter();
            delete this;
        }

    protected:
        // The state required for all requests
        UnaryAndSingleStreamClient& parent_;
        ::grpc::ClientContext ctx_;
    };

    /*! Implementation for the `GetFeature()` RPC request.
     */
    class GetFeatureRequest : public RequestBase {
    public:
        GetFeatureRequest(UnaryAndSingleStreamClient& parent)
            : RequestBase(parent) {

            // Initiate the async request.
            rpc_ = parent_.stub_->AsyncGetFeature(&ctx_, req_, &parent_.cq_);
            assert(rpc_);

            // Add the operation to the queue, so we get notified when
            // the request is completed.
            // Note that we use our handle's this as tag. We don't really need the
            // handle in this unary call, but the server implementation need's
            // to iterate over a Handle to deal with the other reqest classes.
            rpc_->Finish(&reply_, &status_, handle.tag());

            // Reference-counting of instances of requests in flight
            parent.incCounter();
        }

        void proceed(bool ok, Handle::Operation /*op */) {
            if (!ok) [[unlikely]] {
                LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                         << " - The request failed. Status: " << status_.error_message();
                return done();
            }

            // Initiate a new request
            parent_.nextRequest();

            if (status_.ok()) {
                LOG_TRACE << boost::typeindex::type_id_runtime(*this).pretty_name()
                          << " - Request successful. Message: " << reply_.name();
            } else {
                LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                         << " - The request failed with error-message: " << status_.error_message();
            }

            // The reply is a single message, so at this time we are done.
            done();
        }

    private:
        Handle handle{*this, Handle::Operation::CONNECT};

        // We need quite a few variables to perform our single RPC call.
        ::routeguide::Point req_;
        ::routeguide::Feature reply_;
        ::grpc::Status status_;
        std::unique_ptr< ::grpc::ClientAsyncResponseReader< ::routeguide::Feature>> rpc_;
        ::grpc::ClientContext ctx_;
    };


    /*! Implementation for the `ListFeatures()` RPC request.
     */
    class ListFeaturesRequest : public RequestBase {
    public:
        enum class State {
            CREATED,
            CONNECTING,
            READING,
            FINISHED
        };

        // Now we are implementing an actual, trivial state-machine, as
        // we will return an unknown number of messages.

        ListFeaturesRequest(UnaryAndSingleStreamClient& parent)
            : RequestBase(parent) {

            // Initiate the async request.
            // Note that this time, we have to supply the tag to the gRPC initiation method.
            // That's because we will get an event that the request is in progress
            // before we should (can?) start reading the replies.
            rpc_ = parent_.stub_->AsyncListFeatures(&ctx_, req_, &parent_.cq_, connect_handle.tag());
            assert(rpc_);
            state_ = State::CONNECTING;

            // Also register a Finish handler, so we know when we are
            // done or failed. This is where we get the server's status when deal with
            // streams.
            // Note that if we have registered a read-operation,
            // Finish will be called first. Then the read-operation
            // will be called with ok == false;
            // This means that we canot call done() when we get a finish-event,
            // if we are waiting for a read to complete.
            rpc_->Finish(&status_, finish_handle.tag());

            // Reference-counting of instances of requests in flight
            parent.incCounter();
        }

        // As promised, the state-machine get's more complex when we have
        // streams. In this case, we have three states to deal with on each invocation:
        // 1) The state of the instance.
        // 2) The operation
        // 3) The ok boolean value.
        void proceed(bool ok, Handle::Operation op) override {
            switch(op) {
            case Handle::Operation::CONNECT:
                if (!ok) [[unlikely]] {
                    LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                             << " - The request failed. Status: " << status_.error_message();
                    return done();
                }

                LOG_TRACE << boost::typeindex::type_id_runtime(*this).pretty_name()
                          << " - a new request is in progress.";

                // Now, register a read operation.
                rpc_->Read(&reply_, read_handle.tag());
                state_ = State::READING;
                break;

            case Handle::Operation::READ:
                if (!ok) [[unlikely]] {
                    if (state_ == State::FINISHED) {
                        LOG_TRACE << boost::typeindex::type_id_runtime(*this).pretty_name()
                                  << " - I got the failed READ I was waiting for. I'm done now. Promise...";
                        return done();
                    }

                    LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                             << " - Failed to read a message. Status: " << status_.error_message();
                    //return done();
                }

                // This is where we have an actual message from the server.
                // If this was a framework, this is where we would have called
                // `onListFeatureReceivedOneMessage()` or or unblocked the next statement
                // in a co-routine waiting for the next request

                // In our case, let's just log it.
                LOG_TRACE << boost::typeindex::type_id_runtime(*this).pretty_name()
                          << " - Request successful. Message: " << reply_.name();


                // Prepare the reply-object to be re-used.
                // This is usually cheaper than creating a new one for each read operation.
                reply_.Clear();

                // Now, lets register another read operation
                rpc_->Read(&reply_, read_handle.tag());
                break;

            case Handle::Operation::FINISH:                
                if (!ok) [[unlikely]] {
                    LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                             << " - Failed to FINISH! Status: " << status_.error_message();
                    return done();
                }

                if (!status_.ok()) {
                    LOG_WARN << boost::typeindex::type_id_runtime(*this).pretty_name()
                         << " - The request finished with error-message: " << status_.error_message();
                }

                LOG_TRACE << boost::typeindex::type_id_runtime(*this).pretty_name()
                          << " - finishing.";

                // Initiate a new request
                parent_.nextRequest();

                if (state_ != State::READING) {
                    return done(); // There will be no more events
                }

                state_ = State::FINISHED;
                break;

            default:
                LOG_ERROR << boost::typeindex::type_id_runtime(*this).pretty_name()
                          << " - Unexpected operation in state-machine: "
                          << static_cast<int>(op);

                assert(false);

            } // state
        }

    private:
        // We need quite a few variables to perform our single RPC call.
        State state_ = State::CREATED;

        Handle connect_handle   {*this, Handle::Operation::CONNECT};
        Handle read_handle      {*this, Handle::Operation::READ};
        Handle finish_handle    {*this, Handle::Operation::FINISH};

        ::routeguide::Rectangle req_;
        ::routeguide::Feature reply_;
        ::grpc::Status status_;
        std::unique_ptr< ::grpc::ClientAsyncReader< ::routeguide::Feature>> rpc_;
        ::grpc::ClientContext ctx_;
    };

    UnaryAndSingleStreamClient(const Config& config)
        : config_{config} {}

    // Run the event-loop.
    // Returns when there are no more requests to send
    void run(const std::string& serverAddress) {

        LOG_INFO << "Connecting to gRPC service at: " << serverAddress;
        channel_ = grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());

        // Is it a "lame channel"?
        // In stead of returning an empty object if something went wrong,
        // the gRPC team decided it was a better idea to return a valid object with
        // an invalid state that will fail any real operations.
        if (auto status = channel_->GetState(false); status == GRPC_CHANNEL_TRANSIENT_FAILURE) {
            LOG_TRACE << "run - Failed to initialize channel. Is the server address even valid?";
            return;
        }

        stub_ = ::routeguide::RouteGuide::NewStub(channel_);
        assert(stub_);

        // Add request(s)
        LOG_DEBUG << "Creating " << config_.parallel_requests
                  << " initial request(s) of type " << config_.request_type;

        for(auto i = 0; i < config_.parallel_requests;  ++i) {
            nextRequest();
        }

        while(pending_requests_) {
            // FIXME: This is crazy. Figure out how to use stable clock!
            const auto deadline = std::chrono::system_clock::now()
                                  + std::chrono::milliseconds(500);

            // Get any IO operation that is ready.
            void * tag = {};
            bool ok = true;

            // Wait for the next event to complete in the queue
            const auto status = cq_.AsyncNext(&tag, &ok, deadline);

            // So, here we deal with the first of the three states: The status of Next().
            switch(status) {
            case grpc::CompletionQueue::NextStatus::TIMEOUT:
                LOG_DEBUG << "AsyncNext() timed out.";
                continue;

            case grpc::CompletionQueue::NextStatus::GOT_EVENT:
                LOG_TRACE << "AsyncNext() returned an event. The boolean status is "
                          << (ok ? "OK" : "FAILED");

                // Use a scope to allow a new variable inside a case stat`ement.
                {
                    auto handle = static_cast<RequestBase::Handle *>(tag);

                    // Now, let the relevant state-machine deal with the event.
                    // We could have done it here, but that code would smell **really** bad!
                    handle->proceed(ok);
                }
                break;

            case grpc::CompletionQueue::NextStatus::SHUTDOWN:
                LOG_INFO << "SHUTDOWN. Tearing down the gRPC connection(s) ";
                return;
            } // switch
        } // event-loop

        close();
    }

    void close() {
        // Make sure we don't close more than one time.
        // gRPC libraries are not well prepared for surprises ;)
        std::call_once(shutdown_, [this]{
            cq_.Shutdown();
        });
    }

    void nextRequest() {
        static const std::array<std::function<void()>, 2> request_variants = {
            [this]{createRequest<GetFeatureRequest>();},
            [this]{createRequest<ListFeaturesRequest>();}
        };

        request_variants.at(config_.request_type)();
    }

    void incCounter() {
        ++pending_requests_;
    }

    void decCounter() {
        assert(pending_requests_ >= 1);
        --pending_requests_;
    }

private:
    template <typename T>
    void createRequest() {
        if (++request_count > config_.num_requests) {
            LOG_TRACE << "We have already started " << config_.num_requests << " requests.";
            return; // We are done
        }

        try {
            auto instance = std::make_unique<T>(*this);
            instance.release();
        } catch (const std::exception& ex) {
            LOG_ERROR << "Got exception while creating a new instance. Error: "
                      << ex.what();
        }
    }

    // This is the Queue. It's shared for all the requests.
    ::grpc::CompletionQueue cq_;

    // This is a connection to the gRPC server
    std::shared_ptr<grpc::Channel> channel_;

    // An instance of the client that was generated from our .proto file.
    std::unique_ptr<::routeguide::RouteGuide::Stub> stub_;

    std::atomic_size_t pending_requests_{0};
    std::atomic_size_t request_count{0};
    const Config config_;
    std::once_flag shutdown_;
};
