#ifndef UA_SUBSCRIPTION_H_
#define UA_SUBSCRIPTION_H_

#include "ua_util.h"
#include "ua_types.h"
#include "ua_types_generated.h"
#include "ua_nodes.h"
#include "ua_session.h"

/*****************/
/* MonitoredItem */
/*****************/

typedef enum {
    MONITOREDITEM_TYPE_CHANGENOTIFY = 1,
    MONITOREDITEM_TYPE_STATUSNOTIFY = 2,
    MONITOREDITEM_TYPE_EVENTNOTIFY = 4
} UA_MONITOREDITEM_TYPE;

typedef struct MonitoredItem_queuedValue {
    TAILQ_ENTRY(MonitoredItem_queuedValue) listEntry;
    UA_DataValue value;
} MonitoredItem_queuedValue;

typedef struct UA_MonitoredItem {
    LIST_ENTRY(UA_MonitoredItem) listEntry;
    UA_Guid sampleJobGuid;
    UA_Boolean sampleJobIsRegistered;
    UA_UInt32 itemId;
    UA_MONITOREDITEM_TYPE monitoredItemType;
    UA_UInt32 timestampsToReturn;
    UA_MonitoringMode monitoringMode;
    UA_NodeId monitoredNodeId; 
    UA_UInt32 attributeID;
    UA_UInt32 clientHandle;
    UA_Double samplingInterval; // [ms]
    UA_UInt32 currentQueueSize;
    UA_UInt32 maxQueueSize;
    UA_Boolean discardOldest;
    UA_ByteString lastSampledValue;
    // FIXME: indexRange is ignored; array values default to element 0
    // FIXME: dataEncoding is hardcoded to UA binary
    TAILQ_HEAD(QueueOfQueueDataValues, MonitoredItem_queuedValue) queue;
} UA_MonitoredItem;

UA_MonitoredItem *UA_MonitoredItem_new(void);
void MonitoredItem_delete(UA_Server *server, UA_MonitoredItem *monitoredItem);
void MonitoredItem_QueuePushDataValue(UA_Server *server, UA_MonitoredItem *monitoredItem);

UA_StatusCode MonitoredItem_unregisterSampleJob(UA_Server *server, UA_MonitoredItem *mon);
UA_StatusCode MonitoredItem_registerSampleJob(UA_Server *server, UA_MonitoredItem *mon);

/****************/
/* Subscription */
/****************/

typedef struct UA_unpublishedNotification {
    UA_Boolean publishedOnce;
    LIST_ENTRY(UA_unpublishedNotification) listEntry;
    UA_NotificationMessage notification;
} UA_unpublishedNotification;

struct UA_Subscription {
    LIST_ENTRY(UA_Subscription) listEntry;
    UA_Session *session;
    UA_UInt32 lifeTime;
    UA_UInt32 currentKeepAliveCount;
    UA_UInt32 maxKeepAliveCount;
    UA_Double publishingInterval;     // [ms] 
    UA_DateTime lastPublished;
    UA_UInt32 subscriptionID;
    UA_UInt32 notificationsPerPublish;
    UA_Boolean publishingMode;
    UA_UInt32 priority;
    UA_UInt32 sequenceNumber;
    UA_Guid timedUpdateJobGuid;
    UA_Boolean timedUpdateIsRegistered;
    LIST_HEAD(UA_ListOfUnpublishedNotifications, UA_unpublishedNotification) unpublishedNotifications;
    size_t unpublishedNotificationsSize;
    LIST_HEAD(UA_ListOfUAMonitoredItems, UA_MonitoredItem) MonitoredItems;
};

UA_Subscription *UA_Subscription_new(UA_Session *session, UA_UInt32 subscriptionID);
void UA_Subscription_deleteMembers(UA_Subscription *subscription, UA_Server *server);
void Subscription_updateNotifications(UA_Subscription *subscription);
UA_UInt32 *Subscription_getAvailableSequenceNumbers(UA_Subscription *sub);
void Subscription_generateKeepAlive(UA_Subscription *subscription);
UA_UInt32 Subscription_deleteUnpublishedNotification(UA_UInt32 seqNo, UA_Boolean bDeleteAll, UA_Subscription *sub);
void Subscription_copyNotificationMessage(UA_NotificationMessage *dst, UA_unpublishedNotification *src);
UA_StatusCode Subscription_registerUpdateJob(UA_Server *server, UA_Subscription *sub);
UA_StatusCode Subscription_unregisterUpdateJob(UA_Server *server, UA_Subscription *sub);

#endif /* UA_SUBSCRIPTION_H_ */
