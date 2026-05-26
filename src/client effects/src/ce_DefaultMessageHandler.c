//
// ce_DefaultMessageHandler.c
//
// Copyright 1998 Raven Software
//

#include "ce_DefaultMessageHandler.h"
#include "ce_Entities.h"

void CE_DefaultMsgHandler(struct client_entity_s* self_, CE_Message_t* msg)
{
	client_entity_t* self = (client_entity_t*)self_;
	const CE_MsgReceiver_t receiver = ce_class_statics[self->classID].msgReceivers[msg->ID];

	if (receiver != NULL)
		receiver((struct client_entity_s*)self, msg);
}