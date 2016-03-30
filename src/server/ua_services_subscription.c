#include "ua_server_internal.h"
#include "ua_services.h"
#include "ua_subscription.h"

#define UA_BOUNDEDVALUE_SETWBOUNDS(BOUNDS, SRC, DST) { \
    if(SRC > BOUNDS.max) DST = BOUNDS.max; \
    else if(SRC < BOUNDS.min) DST = BOUNDS.min; \
    else DST = SRC; \
    }

static void
setSubscriptionSettings(UA_Server *server, UA_Subscription *subscription,
                        UA_Double requestedPublishingInterval,
                        UA_UInt32 requestedLifetimeCount,
                        UA_UInt32 requestedMaxKeepAliveCount,
                        UA_UInt32 maxNotificationsPerPublish, UA_Byte priority) {
    Subscription_unregisterUpdateJob(server, subscription);
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.publishingIntervalLimits,
                               requestedPublishingInterval, subscription->publishingInterval);
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.lifeTimeCountLimits,
                               requestedLifetimeCount, subscription->lifeTime);
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.keepAliveCountLimits,
                               requestedMaxKeepAliveCount, subscription->maxKeepAliveCount);
    subscription->notificationsPerPublish = maxNotificationsPerPublish;
    subscription->priority = priority;
    Subscription_registerUpdateJob(server, subscription);
}

void Service_CreateSubscription(UA_Server *server, UA_Session *session,
                                const UA_CreateSubscriptionRequest *request,
                                UA_CreateSubscriptionResponse *response) {
    response->subscriptionId = UA_Session_getUniqueSubscriptionID(session);
    UA_Subscription *newSubscription = UA_Subscription_new(session, response->subscriptionId);
    if(!newSubscription) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    UA_Session_addSubscription(session, newSubscription);    
    newSubscription->publishingMode = request->publishingEnabled;
    setSubscriptionSettings(server, newSubscription, request->requestedPublishingInterval,
                            request->requestedLifetimeCount, request->requestedMaxKeepAliveCount,
                            request->maxNotificationsPerPublish, request->priority);
    response->revisedPublishingInterval = newSubscription->publishingInterval;
    response->revisedLifetimeCount = newSubscription->lifeTime;
    response->revisedMaxKeepAliveCount = newSubscription->maxKeepAliveCount;
}

void Service_ModifySubscription(UA_Server *server, UA_Session *session,
                                const UA_ModifySubscriptionRequest *request,
                                UA_ModifySubscriptionResponse *response) {
    UA_Subscription *sub = UA_Session_getSubscriptionByID(session, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    setSubscriptionSettings(server, sub, request->requestedPublishingInterval,
                            request->requestedLifetimeCount, request->requestedMaxKeepAliveCount,
                            request->maxNotificationsPerPublish, request->priority);
    response->revisedPublishingInterval = sub->publishingInterval;
    response->revisedLifetimeCount = sub->lifeTime;
    response->revisedMaxKeepAliveCount = sub->maxKeepAliveCount;
    return;
}

static void
setMonitoredItemSettings(UA_Server *server, UA_MonitoredItem *mon,
                         UA_MonitoringMode monitoringMode, UA_UInt32 clientHandle,
                         UA_Double samplingInterval, UA_UInt32 queueSize, UA_Boolean discardOldest) {
    MonitoredItem_unregisterSampleJob(server, mon);
    mon->monitoringMode = monitoringMode;
    mon->clientHandle = clientHandle;
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.samplingIntervalLimits,
                               samplingInterval, mon->samplingInterval);
    UA_BOUNDEDVALUE_SETWBOUNDS(server->config.queueSizeLimits,
                               queueSize, mon->maxQueueSize);
    mon->discardOldest = discardOldest;
    MonitoredItem_registerSampleJob(server, mon);
}

static void
createMonitoredItem(UA_Server *server, UA_Session *session, UA_Subscription *sub,
                    const UA_MonitoredItemCreateRequest *request,
                    UA_MonitoredItemCreateResult *result) {
    const UA_Node *target = UA_NodeStore_get(server->nodestore, &request->itemToMonitor.nodeId);
    if(!target) {
        result->statusCode = UA_STATUSCODE_BADNODEIDINVALID;
        return;
    }

    UA_MonitoredItem *newMon = UA_MonitoredItem_new();
    if(!newMon) {
        result->statusCode = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }

    UA_StatusCode retval = UA_NodeId_copy(&target->nodeId, &newMon->monitoredNodeId);
    if(retval != UA_STATUSCODE_GOOD) {
        result->statusCode = retval;
        MonitoredItem_delete(server, newMon);
        return;
    }

    newMon->attributeID = request->itemToMonitor.attributeId;
    newMon->itemId = UA_Session_getUniqueSubscriptionID(session);
    setMonitoredItemSettings(server, newMon, MONITOREDITEM_TYPE_CHANGENOTIFY,
                             request->requestedParameters.clientHandle,
                             request->requestedParameters.samplingInterval,
                             request->requestedParameters.queueSize,
                             request->requestedParameters.discardOldest);

    result->revisedSamplingInterval = newMon->samplingInterval;
    result->revisedQueueSize = newMon->maxQueueSize;
    result->monitoredItemId = newMon->itemId;
    LIST_INSERT_HEAD(&sub->MonitoredItems, newMon, listEntry);
    // todo: add a pointer to the monitoreditem to the variable, so that events get propagated
}

void Service_CreateMonitoredItems(UA_Server *server, UA_Session *session,
                                  const UA_CreateMonitoredItemsRequest *request,
                                  UA_CreateMonitoredItemsResponse *response) {
    UA_Subscription *sub = UA_Session_getSubscriptionByID(session, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    if(request->itemsToCreateSize <= 0) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADNOTHINGTODO;
        return;
    }

    response->results = UA_Array_new(request->itemsToCreateSize, &UA_TYPES[UA_TYPES_MONITOREDITEMCREATERESULT]);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->itemsToCreateSize;

    for(size_t i = 0; i < request->itemsToCreateSize; i++)
        createMonitoredItem(server, session, sub, &request->itemsToCreate[i], &response->results[i]);
}

void
Service_Publish(UA_Server *server, UA_Session *session, const UA_PublishRequest *request,
                UA_UInt32 requestId) {
    UA_PublishResponse response;
    UA_PublishResponse_init(&response);
    response.responseHeader.requestHandle = request->requestHeader.requestHandle;
    response.responseHeader.timestamp = UA_DateTime_now();
    
    // Delete Acknowledged Subscription Messages
    response.resultsSize = request->subscriptionAcknowledgementsSize;
    response.results = UA_calloc(response.resultsSize, sizeof(UA_StatusCode));
    for(size_t i = 0; i < request->subscriptionAcknowledgementsSize; i++) {
        response.results[i] = UA_STATUSCODE_GOOD;
        UA_UInt32 sid = request->subscriptionAcknowledgements[i].subscriptionId;
        UA_Subscription *sub = UA_Session_getSubscriptionByID(session, sid);
        if(!sub) {
            response.results[i] = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
            continue;
        }
        UA_UInt32 sn = request->subscriptionAcknowledgements[i].sequenceNumber;
        if(Subscription_deleteUnpublishedNotification(sn, false, sub) == 0)
            response.results[i] = UA_STATUSCODE_BADSEQUENCENUMBERINVALID;
    }
    
    UA_Boolean have_response = false;

    // See if any new data is available
    UA_Subscription *sub;
    LIST_FOREACH(sub, &session->serverSubscriptions, listEntry) {
        if(sub->timedUpdateIsRegistered == false) {
            // FIXME: We are forcing a value update for monitored items. This should be done by the event system.
            // NOTE:  There is a clone of this functionality in the Subscription_timedUpdateNotificationsJob
            // done by the sampling job
            /* UA_MonitoredItem *mon; */
            /* LIST_FOREACH(mon, &sub->MonitoredItems, listEntry) */
            /*     MonitoredItem_QueuePushDataValue(server, mon); */
            
            // FIXME: We are forcing notification updates for the subscription. This
            // should be done by a timed work item.
            Subscription_updateNotifications(sub);
        }
        
        if(sub->unpublishedNotificationsSize == 0)
            continue;
        
        // This subscription has notifications in its queue (top NotificationMessage exists in the queue). 
        // Due to republish, we need to check if there are any unplublished notifications first ()
        UA_unpublishedNotification *notification = NULL;
        LIST_FOREACH(notification, &sub->unpublishedNotifications, listEntry) {
            if (notification->publishedOnce == false)
                break;
        }
        if (notification == NULL)
            continue;
    
        // We found an unpublished notification message in this subscription, which we will now publish.
        response.subscriptionId = sub->subscriptionID;
        Subscription_copyNotificationMessage(&response.notificationMessage, notification);
        // Mark this notification as published
        notification->publishedOnce = true;
        if(notification->notification.sequenceNumber > sub->sequenceNumber) {
            // If this is a keepalive message, its seqNo is the next seqNo to be used for an actual msg.
            response.availableSequenceNumbersSize = 0;
            // .. and must be deleted
            Subscription_deleteUnpublishedNotification(sub->sequenceNumber + 1, false, sub);
        } else {
            response.availableSequenceNumbersSize = sub->unpublishedNotificationsSize;
            response.availableSequenceNumbers = Subscription_getAvailableSequenceNumbers(sub);
        }	  
        have_response = true;
    }
    
    if(!have_response) {
        // FIXME: At this point, we would return nothing and "queue" the publish
        // request, but currently we need to return something to the client. If no
        // subscriptions have notifications, force one to generate a keepalive so we
        // don't return an empty message
        sub = LIST_FIRST(&session->serverSubscriptions);
        if(sub) {
            response.subscriptionId = sub->subscriptionID;
            Subscription_generateKeepAlive(sub);
            Subscription_copyNotificationMessage(&response.notificationMessage,
                                                 LIST_FIRST(&sub->unpublishedNotifications));
            Subscription_deleteUnpublishedNotification(sub->sequenceNumber + 1, false, sub);
        }
    }
    
    UA_SecureChannel *channel = session->channel;
    if(channel)
        UA_SecureChannel_sendBinaryMessage(channel, requestId, &response,
                                           &UA_TYPES[UA_TYPES_PUBLISHRESPONSE]);
    UA_PublishResponse_deleteMembers(&response);
}

void Service_DeleteSubscriptions(UA_Server *server, UA_Session *session,
                                 const UA_DeleteSubscriptionsRequest *request,
                                 UA_DeleteSubscriptionsResponse *response) {
    response->results = UA_malloc(sizeof(UA_StatusCode) * request->subscriptionIdsSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->subscriptionIdsSize;

    for(size_t i = 0; i < request->subscriptionIdsSize; i++)
        response->results[i] = UA_Session_deleteSubscription(server, session, request->subscriptionIds[i]);
} 

void Service_DeleteMonitoredItems(UA_Server *server, UA_Session *session,
                                  const UA_DeleteMonitoredItemsRequest *request,
                                  UA_DeleteMonitoredItemsResponse *response) {
    UA_Subscription *sub = UA_Session_getSubscriptionByID(session, request->subscriptionId);
    if(!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    response->results = UA_malloc(sizeof(UA_StatusCode) * request->monitoredItemIdsSize);
    if(!response->results) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADOUTOFMEMORY;
        return;
    }
    response->resultsSize = request->monitoredItemIdsSize;

    for(size_t i = 0; i < request->monitoredItemIdsSize; i++)
        response->results[i] =
            UA_Session_deleteMonitoredItem(server, session, sub->subscriptionID, request->monitoredItemIds[i]);
}

void Service_Republish(UA_Server *server, UA_Session *session, const UA_RepublishRequest *request,
                       UA_RepublishResponse *response) {
    UA_Subscription *sub = UA_Session_getSubscriptionByID(session, request->subscriptionId);
    if (!sub) {
        response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
        return;
    }
    
    // Find the notification in question
    UA_unpublishedNotification *notification;
    LIST_FOREACH(notification, &sub->unpublishedNotifications, listEntry) {
        if(notification->notification.sequenceNumber == request->retransmitSequenceNumber)
            break;
    }
    if(!notification) {
      response->responseHeader.serviceResult = UA_STATUSCODE_BADSEQUENCENUMBERINVALID;
      return;
    }
    
    // FIXME: By spec, this notification has to be in the "retransmit queue", i.e. publishedOnce must be
    //        true. If this is not tested, the client just gets what he asks for... hence this part is
    //        commented:
    /* Check if the notification is in the published queue
    if (notification->publishedOnce == false) {
      response->responseHeader.serviceResult = UA_STATUSCODE_BADSUBSCRIPTIONIDINVALID;
      return;
    }
    */
    // Retransmit 
    Subscription_copyNotificationMessage(&response->notificationMessage, notification);
    // Mark this notification as published
    notification->publishedOnce = true;
    
    return;
}
