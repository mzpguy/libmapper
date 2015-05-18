#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

#include <lo/lo.h>

#include "mapper_internal.h"
#include "types_internal.h"
#include <mapper/mapper.h>

static void send_or_bundle_message(mapper_link link, const char *path,
                                   lo_message m, mapper_timetag_t tt);

static int get_in_scope(mapper_connection c, uint32_t name_hash)
{
    int i;
    for (i=0; i<c->props.scope.size; i++) {
        if (c->props.scope.hashes[i] == name_hash ||
            c->props.scope.hashes[i] == 0)
            return 1;
    }
    return 0;
}

static void mapper_link_free(mapper_link l)
{
    if (l) {
        if (l->props.host)
            free(l->props.host);
        if (l->props.name)
            free(l->props.name);
        if (l->admin_addr)
            lo_address_free(l->admin_addr);
        if (l->data_addr)
            lo_address_free(l->data_addr);
        while (l->queues) {
            mapper_queue q = l->queues;
            lo_bundle_free_messages(q->bundle);
            l->queues = q->next;
            free(q);
        }
        free(l);
    }
}

mapper_link mapper_router_add_link(mapper_router router, const char *host,
                                   int admin_port, int data_port,
                                   const char *name)
{
    if (!name)
        return 0;

    char str[16];
    mapper_link l = (mapper_link) calloc(1, sizeof(struct _mapper_link));
    if (host) {
        l->props.host = strdup(host);
        l->props.port = data_port;
        snprintf(str, 16, "%d", data_port);
        l->data_addr = lo_address_new(host, str);
        snprintf(str, 16, "%d", admin_port);
        l->admin_addr = lo_address_new(host, str);
    }
    name += (name[0]=='/');
    l->props.name = strdup(name);
    l->props.name_hash = crc32(0L, (const Bytef *)name, strlen(name));

    l->device = router->device;
    l->props.num_connections_in = 0;
    l->props.num_connections_out = 0;

    if (name == mdev_name(router->device)) {
        /* Add data_addr for use by self-connections. In the future we may
         * decide to call local handlers directly, however this could result in
         * unfortunate loops/stack overflow. Sending data for self-connections
         * to localhost adds the messages to liblo's stack and imposes a delay
         * since the receiving handler will not be called until mdev_poll(). */
        snprintf(str, 16, "%d", router->device->props.port);
        l->data_addr = lo_address_new("localhost", str);
        l->self_link = 1;
    }

    l->clock.new = 1;
    l->clock.sent.message_id = 0;
    l->clock.response.message_id = -1;
    mapper_clock_t *clock = &router->device->admin->clock;
    mapper_clock_now(clock, &clock->now);
    l->clock.response.timetag.sec = clock->now.sec + 10;

    l->next = router->links;
    router->links = l;

    if (!host) {
        // request missing metadata
        char cmd[256];
        snprintf(cmd, 256, "/%s/subscribe", name);
        lo_message m = lo_message_new();
        if (m) {
            lo_message_add_string(m, "device");
            mapper_admin_set_bundle_dest_bus(router->device->admin);
            lo_bundle_add_message(router->device->admin->bundle, cmd, m);
            mapper_admin_send_bundle(router->device->admin);
        }
    }

    return l;
}

void mapper_router_update_link(mapper_router router, mapper_link link,
                               const char *host, int admin_port, int data_port)
{
    char str[16];
    link->props.host = strdup(host);
    link->props.port = data_port;
    sprintf(str, "%d", data_port);
    link->data_addr = lo_address_new(host, str);
    sprintf(str, "%d", admin_port);
    link->admin_addr = lo_address_new(host, str);
}

void mapper_router_remove_link(mapper_router router, mapper_link link)
{
    int i, j;
    // check if any connection use this link
    mapper_router_signal rs = router->signals;
    while (rs) {
        for (i = 0; i < rs->num_slots; i++) {
            if (!rs->slots[i])
                continue;
            mapper_connection c = rs->slots[i]->connection;
            if (c->destination.link == link) {
                mapper_router_remove_connection(router, c);
                continue;
            }
            for (j = 0; j < c->props.num_sources; j++) {
                if (c->sources[j].link == link) {
                    mapper_router_remove_connection(router, c);
                    break;
                }
            }
        }
        rs = rs->next;
    }
    mapper_link *l = &router->links;
    while (*l) {
        if (*l == link) {
            *l = (*l)->next;
            break;
        }
        l = &(*l)->next;
    }
    mapper_link_free(link);
}

static void reallocate_slot_instances(mapper_connection_slot s, int size)
{
    int i;
    if (s->props->num_instances < size) {
        s->history = realloc(s->history, sizeof(struct _mapper_history) * size);
        for (i = s->props->num_instances; i < size; i++) {
            s->history[i].type = s->props->type;
            s->history[i].length = s->props->length;
            s->history[i].size = s->history_size;
            s->history[i].value = calloc(1, mapper_type_size(s->props->type)
                                         * s->history_size);
            s->history[i].timetag = calloc(1, sizeof(mapper_timetag_t)
                                           * s->history_size);
            s->history[i].position = -1;
        }
        s->props->num_instances = size;
    }
}

static void reallocate_connection_instances(mapper_connection c, int size)
{
    int i, j;
    if (!(c->status & MAPPER_TYPE_KNOWN) || !(c->status & MAPPER_LENGTH_KNOWN)) {
        for (i = 0; i < c->props.num_sources; i++) {
            c->sources[i].props->num_instances = size;
        }
        c->destination.props->num_instances = size;
        return;
    }

    // check if source histories need to be reallocated
    for (i = 0; i < c->props.num_sources; i++)
        reallocate_slot_instances(&c->sources[i], size);

    // check if destination histories need to be reallocated
    reallocate_slot_instances(&c->destination, size);

    // check if expression variable histories need to be reallocated
    if (size > c->num_var_instances) {
        c->expr_vars = realloc(c->expr_vars, sizeof(mapper_history*) * size);
        for (i = c->num_var_instances; i < size; i++) {
            c->expr_vars[i] = malloc(sizeof(struct _mapper_history)
                                     * c->num_expr_vars);
            for (j = 0; j < c->num_expr_vars; j++) {
                c->expr_vars[i][j].type = c->expr_vars[0][j].type;
                c->expr_vars[i][j].length = c->expr_vars[0][j].length;
                c->expr_vars[i][j].size = c->expr_vars[0][j].size;
                c->expr_vars[i][j].position = -1;
                c->expr_vars[i][j].value = calloc(1, sizeof(double)
                                                  * c->expr_vars[i][j].length
                                                  * c->expr_vars[i][j].size);
                c->expr_vars[i][j].timetag = calloc(1, sizeof(mapper_timetag_t)
                                                    * c->expr_vars[i][j].size);
            }
        }
        c->num_var_instances = size;
    }
}

// TODO: check for mismatched instance counts when using multiple sources
void mapper_router_num_instances_changed(mapper_router r,
                                         mapper_signal sig,
                                         int size)
{
    int i;
    // check if we have a reference to this signal
    mapper_router_signal rs = r->signals;
    while (rs) {
        if (rs->signal == sig)
            break;
        rs = rs->next;
    }

    if (!rs) {
        // The signal is not mapped through this router.
        return;
    }

    // for array of slots, may need to reallocate destination instances
    for (i = 0; i < rs->num_slots; i++) {
        mapper_connection_slot s = rs->slots[i];
        if (s)
            reallocate_connection_instances(s->connection, size);
    }
}

void mapper_router_process_signal(mapper_router r, mapper_signal sig,
                                  int instance, void *value, int count,
                                  mapper_timetag_t tt)
{
    mapper_id_map map = sig->id_maps[instance].map;
    lo_message m;

    // find the signal connection
    mapper_router_signal rs = r->signals;
    while (rs) {
        if (rs->signal == sig)
            break;
        rs = rs->next;
    }
    if (!rs)
        return;

    int i, j, k, id = sig->id_maps[instance].instance->index;
    mapper_connection c;

    if (!value) {
        mapper_connection_slot s;
        mapper_db_connection_slot p;

        for (i = 0; i < rs->num_slots; i++) {
            if (!rs->slots[i])
                continue;

            s = rs->slots[i];
            c = s->connection;

            if (c->status < MAPPER_ACTIVE)
                continue;

            if (s->props->direction == DI_OUTGOING) {
                mapper_connection_slot ds = &c->destination;
                mapper_db_connection_slot dp = &c->props.destination;

                // also need to reset associated output memory
                ds->history[id].position= -1;
                memset(ds->history[id].value, 0, ds->history_size
                       * dp->length * mapper_type_size(dp->type));
                memset(ds->history[id].timetag, 0, ds->history_size
                       * sizeof(mapper_timetag_t));

                if (!s->props->send_as_instance)
                    m = mapper_connection_build_message(c, s, 0, 1, 0, 0);
                else if (get_in_scope(c, map->origin))
                    m = mapper_connection_build_message(c, s, 0, 1, 0, map);
                if (m)
                    send_or_bundle_message(c->destination.link,
                                           dp->signal->path, m, tt);
                continue;
            }
            else if (!get_in_scope(c, map->origin))
                continue;
            for (j = 0; j < c->props.num_sources; j++) {
                if (!c->sources[j].props->send_as_instance)
                    continue;
                s = &c->sources[j];
                p = &c->props.sources[j];
                // send release to upstream
                m = mapper_connection_build_message(c, s, 0, 1, 0, map);
                if (m)
                    send_or_bundle_message(s->link, p->signal->path, m, tt);

                // also need to reset associated input memory
                memset(s->history[id].value, 0, s->history_size
                       * p->length * mapper_type_size(p->type));
                memset(s->history[id].timetag, 0, s->history_size
                       * sizeof(mapper_timetag_t));
            }
        }
        return;
    }

    // if count > 1, we need to allocate sufficient memory for largest output
    // vector so that we can store calculated values before sending
    // TODO: calculate max_output_size, cache in link_signal
    void *out_value_p = count == 1 ? 0 : alloca(count * sig->props.length
                                                * sizeof(double));
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i])
            continue;

        mapper_connection_slot s = rs->slots[i];
        c = s->connection;

        if (c->status < MAPPER_ACTIVE)
            continue;

        int in_scope = get_in_scope(c, map->origin);
        // TODO: should we continue for out-of-scope local destinaton updates?
        if (s->props->send_as_instance && !in_scope) {
            continue;
        }

        mapper_db_connection_slot sp = s->props;
        mapper_db_connection_slot dp = &c->props.destination;
        mapper_db_connection_slot to = (c->props.process_location
                                        == MAPPER_SOURCE ? dp : sp);
        int to_size = mapper_type_size(to->type) * to->length;
        char typestring[to->length * count];
        memset(typestring, to->type, to->length * count);
        k = 0;
        for (j = 0; j < count; j++) {
            // copy input history
            size_t n = msig_vector_bytes(sig);
            s->history[id].position = ((s->history[id].position + 1)
                                       % s->history[id].size);
            memcpy(mapper_history_value_ptr(s->history[id]), value + n * j, n);
            memcpy(mapper_history_tt_ptr(s->history[id]),
                   &tt, sizeof(mapper_timetag_t));

            // process source boundary behaviour
            if ((mapper_boundary_perform(&s->history[id], sp,
                                         typestring + to->length * k))) {
                // back up position index
                --s->history[id].position;
                if (s->history[id].position < 0)
                    s->history[id].position = s->history[id].size - 1;
                continue;
            }

            if (sp->direction == DI_INCOMING) {
                continue;
            }

            if (c->props.process_location == MAPPER_SOURCE && !sp->cause_update)
                continue;

            if (!(mapper_connection_perform(c, s, instance,
                                            typestring + to->length * k)))
                continue;

            if (c->props.process_location == MAPPER_SOURCE) {
                // also process destination boundary behaviour
                if ((mapper_boundary_perform(&c->destination.history[id], dp,
                                             typestring + to->length * k))) {
                    // back up position index
                    --c->destination.history[id].position;
                    if (c->destination.history[id].position < 0)
                        c->destination.history[id].position = c->destination.history[id].size - 1;
                    continue;
                }
            }

            void *result = mapper_history_value_ptr(c->destination.history[id]);

            if (count > 1) {
                memcpy((char*)out_value_p + to_size * j, result, to_size);
            }
            else {
                m = mapper_connection_build_message(c, s, result, 1, typestring,
                                                    sp->send_as_instance ? map : 0);
                if (m)
                    send_or_bundle_message(c->destination.link,
                                           dp->signal->path, m, tt);
            }
            k++;
        }
        if (count > 1 && s->props->direction == DI_OUTGOING
            && (!s->props->send_as_instance || in_scope)) {
            m = mapper_connection_build_message(c, s, out_value_p, k, typestring,
                                                sp->send_as_instance ? map : 0);
            if (m)
                send_or_bundle_message(c->destination.link, dp->signal->path, m, tt);
        }
    }
}

int mapper_router_send_query(mapper_router r,
                             mapper_signal sig,
                             mapper_timetag_t tt)
{
    // TODO: cache the response string
    if (!sig->handler) {
        trace("not sending queries since signal has no handler.\n");
        return 0;
    }
    // find this signal in list of connections
    mapper_router_signal rs = r->signals;
    while (rs && rs->signal != sig)
        rs = rs->next;

    // exit without failure if signal is not mapped
    if (!rs)
        return 0;

    // for each connection, query the remote signal
    int i, j, count = 0;
    int len = (int) strlen(sig->props.path) + 5;
    char *response_string = (char*) malloc(len);
    snprintf(response_string, len, "%s/got", sig->props.path);
    char query_string[256];
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i])
            continue;
        mapper_connection c = rs->slots[i]->connection;
        if (c->status != MAPPER_ACTIVE)
            continue;
        lo_message m = lo_message_new();
        if (!m)
            continue;
        lo_message_add_string(m, response_string);
        lo_message_add_int32(m, sig->props.length);
        lo_message_add_char(m, sig->props.type);
        // TODO: include response address as argument to allow TCP queries?
        // TODO: always use TCP for queries?

        if (rs->slots[i]->props->direction == DI_OUTGOING) {
            snprintf(query_string, 256, "%s/get",
                     c->props.destination.signal->path);
            send_or_bundle_message(c->destination.link, query_string, m, tt);
        }
        else {
            for (j = 0; j < c->props.num_sources; j++) {
                snprintf(query_string, 256, "%s/get", c->props.sources[j].signal->path);
                send_or_bundle_message(c->sources[j].link, query_string, m, tt);
            }
        }
        count++;
    }
    free(response_string);
    return count;
}

// note on memory handling of mapper_router_bundle_message():
// path: not owned, will not be freed (assumed is signal name, owned
//       by signal)
// message: will be owned, will be freed when done
void send_or_bundle_message(mapper_link link, const char *path,
                            lo_message m, mapper_timetag_t tt)
{
    // Check if a matching bundle exists
    mapper_queue q = link->queues;
    while (q) {
        if (memcmp(&q->tt, &tt,
                   sizeof(mapper_timetag_t))==0)
            break;
        q = q->next;
    }
    if (q) {
        // Add message to existing bundle
        lo_bundle_add_message(q->bundle, path, m);
    }
    else {
        // Send message immediately
        lo_bundle b = lo_bundle_new(tt);
        lo_bundle_add_message(b, path, m);
        lo_send_bundle_from(link->data_addr, link->device->server, b);
        lo_bundle_free_messages(b);
    }
}

void mapper_router_start_queue(mapper_router r,
                               mapper_timetag_t tt)
{
    // first check if queue already exists
    mapper_link l = r->links;
    while (l) {
        mapper_queue q = l->queues;
        while (q) {
            if (memcmp(&q->tt, &tt,
                       sizeof(mapper_timetag_t))==0)
                return;
            q = q->next;
        }

        // need to create new queue
        q = malloc(sizeof(struct _mapper_queue));
        memcpy(&q->tt, &tt, sizeof(mapper_timetag_t));
        q->bundle = lo_bundle_new(tt);
        q->next = l->queues;
        l->queues = q;
        l = l->next;
    }
}

void mapper_router_send_queue(mapper_router r,
                              mapper_timetag_t tt)
{
    mapper_link l = r->links;
    while (l) {
        mapper_queue *q = &l->queues;
        while (*q) {
            if (memcmp(&(*q)->tt, &tt, sizeof(mapper_timetag_t))==0)
                break;
            q = &(*q)->next;
        }
        if (*q) {
#ifdef HAVE_LIBLO_BUNDLE_COUNT
            if (lo_bundle_count((*q)->bundle))
#endif
                lo_send_bundle_from(l->data_addr,
                                    l->device->server, (*q)->bundle);
            lo_bundle_free_messages((*q)->bundle);
            mapper_queue temp = *q;
            *q = (*q)->next;
            free(temp);
        }
        l = l->next;
    }
}

static mapper_router_signal find_or_add_router_signal(mapper_router r,
                                                      mapper_signal sig)
{
    // find signal in router_signal list
    mapper_router_signal rs = r->signals;
    while (rs && rs->signal != sig)
        rs = rs->next;

    // if not found, create a new list entry
    if (!rs) {
        rs = ((mapper_router_signal)
              calloc(1, sizeof(struct _mapper_router_signal)));
        rs->signal = sig;
        rs->num_slots = 1;
        rs->slots = malloc(sizeof(mapper_connection_slot *));
        rs->slots[0] = 0;
        rs->next = r->signals;
        r->signals = rs;
    }
    return rs;
}

static int router_signal_store_slot(mapper_router_signal rs,
                                    mapper_connection_slot slot)
{
    int i;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i]) {
            // store pointer at empty index
            rs->slots[i] = slot;
            return i;
        }
    }
    // all indices occupied, allocate more
    rs->slots = realloc(rs->slots, sizeof(mapper_connection_slot *)
                        * rs->num_slots * 2);
    rs->slots[rs->num_slots] = slot;
    for (i = rs->num_slots+1; i < rs->num_slots * 2; i++) {
        rs->slots[i] = 0;
    }
    i = rs->num_slots;
    rs->num_slots *= 2;
    return i;
}

mapper_connection mapper_router_add_connection(mapper_router r,
                                               mapper_signal sig,
                                               int num_sources,
                                               mapper_signal *local_signals,
                                               const char **remote_signal_names,
                                               int direction)
{
    int i, ready = 1;

    if (num_sources > MAX_NUM_CONNECTION_SOURCES) {
        trace("error: maximum number of remote signals in a connection exceeded.\n");
        return 0;
    }

    mapper_router_signal rs = find_or_add_router_signal(r, sig);
    mapper_connection c = ((mapper_connection)
                           calloc(1, sizeof(struct _mapper_connection)));
    c->router = r;
    c->status = 0;

    // TODO: configure number of instances available for each slot
    c->num_var_instances = sig->props.num_instances;

    c->props.num_sources = num_sources;
    c->sources = ((mapper_connection_slot)
                  calloc(1, sizeof(struct _mapper_connection_slot)
                         * num_sources));
    c->props.sources = ((mapper_db_connection_slot)
                        calloc(1, sizeof(struct _mapper_db_connection_slot)
                               * num_sources));
    // scopes
    c->props.scope.size = direction == DI_OUTGOING ? 1 : num_sources;
    c->props.scope.names = (char **) malloc(sizeof(char *) * c->props.scope.size);
    c->props.scope.hashes = (uint32_t *) malloc(sizeof(uint32_t)
                                                * c->props.scope.size);

    // is_admin property will be corrected later if necessary
    c->is_admin = 1;
    c->props.id = direction == DI_INCOMING ? r->id_counter++ : -1;

    mapper_link link;
    char devname[256], *devnamep, *signame;
    const char *remote_devname_ptr;
    int devnamelen, scope_count = 0, local_scope = 0;
    mapper_db_connection_slot sp;
    if (direction == DI_OUTGOING) {
        ready = 0;
        for (i = 0; i < num_sources; i++) {
            // find router_signal
            mapper_router_signal rs2 = find_or_add_router_signal(r, local_signals[i]);
            c->sources[i].local = rs2;
            sp = c->sources[i].props = &c->props.sources[i];
            sp->signal = &local_signals[i]->props;
            sp->type = local_signals[i]->props.type;
            sp->length = local_signals[i]->props.length;
            c->sources[i].status = MAPPER_READY;
            sp->direction = DI_OUTGOING;
            sp->num_instances = local_signals[i]->props.num_instances;
            if (sp->num_instances > c->num_var_instances)
                c->num_var_instances = sp->num_instances;

            /* slot index will be overwritten if necessary by
             * mapper_connection_set_from_message() */
            sp->slot_id = -1;

            // also need to add connection to lists kept by source rs
            router_signal_store_slot(rs2, &c->sources[i]);
        }

        devnamelen = mapper_parse_names(remote_signal_names[0],
                                        &devnamep, &signame);
        if (!devnamelen || devnamelen >= 256) {
            // TODO: free partially-built connection structure
            return 0;
        }
        strncpy(devname, devnamep, devnamelen);
        devname[devnamelen] = 0;
        sp = c->destination.props = &c->props.destination;
        sp->signal = calloc(1, sizeof(mapper_db_signal_t));
        int signamelen = strlen(signame)+2;
        sp->signal->path = malloc(signamelen);
        snprintf(sp->signal->path, signamelen, "/%s", signame);
        sp->signal->name = sp->signal->path+1;
        sp->direction = DI_OUTGOING;
        sp->num_instances = sig->props.num_instances;

        link = mapper_router_find_link_by_remote_name(r, devname);
        if (link) {
            c->destination.link = link;
        }
        else
            c->destination.link = mapper_router_add_link(r, 0, 0, 0, devname);
        sp->signal->device = &c->destination.link->props;

        // apply local scope as default
        c->props.scope.names[0] = strdup(mdev_name(r->device));
        c->props.scope.hashes[0] = mdev_id(r->device);
    }
    else {
        c->destination.local = rs;
        sp = c->destination.props = &c->props.destination;
        sp->signal = &sig->props;
        sp->type = sig->props.type;
        sp->length = sig->props.length;
        c->destination.status = MAPPER_READY;
        sp->direction = DI_INCOMING;
        sp->num_instances = sig->props.num_instances;

        router_signal_store_slot(rs, &c->destination);

        for (i = 0; i < num_sources; i++) {
            sp = c->sources[i].props = &c->props.sources[i];
            if (local_signals && local_signals[i]) {
                remote_devname_ptr = mdev_name(r->device);

                // find router_signal
                mapper_router_signal rs2 = find_or_add_router_signal(r, local_signals[i]);
                c->sources[i].local = rs2;
                sp->signal = &local_signals[i]->props;
                sp->type = local_signals[i]->props.type;
                sp->length = local_signals[i]->props.length;
                c->sources[i].status = MAPPER_READY;
                sp->direction = DI_OUTGOING;
                sp->num_instances = local_signals[i]->props.num_instances;

                if (sp->num_instances > c->num_var_instances)
                    c->num_var_instances = sp->num_instances;

                // ensure local scope is only added once
                if (!local_scope) {
                    c->props.scope.names[scope_count] = strdup(remote_devname_ptr);
                    c->props.scope.hashes[scope_count] = crc32(0L,
                                                               (const Bytef *)remote_devname_ptr,
                                                               strlen(devname));
                    scope_count++;
                    local_scope = 1;
                }

                // also need to add connection to lists kept by source rs
                router_signal_store_slot(rs2, &c->sources[i]);
            }
            else {
                ready = 0;
                devnamelen = mapper_parse_names(remote_signal_names[i],
                                                &devnamep, &signame);
                if (!devnamelen || devnamelen >= 256) {
                    // TODO: free partially-built connection structure
                    return 0;
                }
                strncpy(devname, devnamep, devnamelen);
                devname[devnamelen] = 0;
                remote_devname_ptr = devname;
                sp->signal = calloc(1, sizeof(mapper_db_signal_t));
                int signamelen = strlen(signame)+2;
                sp->signal->path = malloc(signamelen);
                snprintf(sp->signal->path, signamelen, "/%s", signame);
                sp->signal->name = sp->signal->path+1;
                sp->direction = DI_INCOMING;
                sp->num_instances = sig->props.num_instances;

                // TODO: check that scope is not added multiple times
                c->props.scope.names[scope_count] = strdup(devname);
                c->props.scope.hashes[scope_count] = crc32(0L,
                                                           (const Bytef *)devname,
                                                           strlen(devname));
                scope_count++;
            }

            link = mapper_router_find_link_by_remote_name(r, remote_devname_ptr);
            if (link) {
                c->sources[i].link = link;
            }
            else
                c->sources[i].link = mapper_router_add_link(r, 0, 0, 0,
                                                            remote_devname_ptr);

            if (!c->sources[i].local)
                sp->signal->device = &c->sources[i].link->props;
            sp->slot_id = rs->id_counter++;
        }
        if (scope_count != num_sources) {
            c->props.scope.size = scope_count;
            c->props.scope.names = realloc(c->props.scope.names,
                                           sizeof(char *) * scope_count);
            c->props.scope.hashes = realloc(c->props.scope.hashes,
                                            sizeof(uint32_t) * scope_count);
        }
    }

    for (i = 0; i < num_sources; i++) {
        c->sources[i].connection = c;
        c->props.sources[i].cause_update = 1;
        c->props.sources[i].send_as_instance = c->props.sources[i].num_instances > 1;
        c->props.sources[i].bound_min = BA_NONE;
        c->props.sources[i].bound_max = BA_NONE;
    }
    c->destination.connection = c;
    c->props.destination.slot_id = -1;
    c->props.destination.cause_update = 1;
    c->props.destination.send_as_instance = c->props.destination.num_instances > 1;
    c->props.destination.bound_min = c->props.destination.bound_max = BA_NONE;
    c->props.destination.minimum = c->props.destination.maximum = 0;

    c->props.muted = 0;
    c->props.mode = MO_UNDEFINED;
    c->props.expression = 0;
    c->props.extra = table_new();

    // check if all sources belong to same remote device
    c->one_source = 1;
    for (i = 1; i < c->props.num_sources; i++) {
        if (c->sources[i].link != c->sources[0].link) {
            c->one_source = 0;
            break;
        }
    }
    // default to processing at source device unless heterogeneous sources
    if (c->one_source)
        c->props.process_location = MAPPER_SOURCE;
    else
        c->props.process_location = MAPPER_DESTINATION;

    if (ready) {
        // all reference signals are local
        c->is_local = 1;
        c->is_admin = 1;
        link = c->sources[0].link;
        c->destination.link = link;
    }
    return c;
}

static void check_link(mapper_router r, mapper_link l)
{
    /* We could remove link the link here if it has no associated connections,
     * however under normal usage it is likely that users will add new
     * connections after deleting the old ones. If we remove the link
     * immediately it will have to be re-established in this scenario, so we
     * will allow the admin house-keeping routines to clean up empty links
     * after the link ping timeout period. */
}

void mapper_router_remove_signal(mapper_router r, mapper_router_signal rs)
{
    if (r && rs) {
        // No connections remaining – we can remove the router_signal also
        mapper_router_signal *rstemp = &r->signals;
        while (*rstemp) {
            if (*rstemp == rs) {
                *rstemp = rs->next;
                free(rs->slots);
                free(rs);
                break;
            }
            rstemp = &(*rstemp)->next;
        }
    }
}

static void free_slot_memory(mapper_connection_slot s)
{
    int i;
    if (s->props->minimum)
        free(s->props->minimum);
    if (s->props->maximum)
        free(s->props->maximum);
    if (!s->local) {
        if (s->props->signal) {
            if (s->props->signal->path)
                free((void*)s->props->signal->path);
            free(s->props->signal);
        }
        if (s->history) {
            for (i = 0; i < s->props->num_instances; i++) {
                free(s->history[i].value);
                free(s->history[i].timetag);
            }
            free(s->history);
        }
    }
}

int mapper_router_remove_connection(mapper_router r, mapper_connection c)
{
    // do not free local names since they point to signal's copy
    int i, j;
    if (!c)
        return 1;

    // remove connection and slots from router_signal lists if necessary
    if (c->destination.local) {
        mapper_router_signal rs = c->destination.local;
        for (i = 0; i < rs->num_slots; i++) {
            if (rs->slots[i] == &c->destination) {
                rs->slots[i] = 0;
                break;
            }
        }
        if (c->status >= MAPPER_READY && c->destination.link) {
            c->destination.link->props.num_connections_in--;
            check_link(r, c->destination.link);
        }
    }
    else if (c->status >= MAPPER_READY && c->destination.link) {
        c->destination.link->props.num_connections_out--;
        check_link(r, c->destination.link);
    }
    free_slot_memory(&c->destination);
    for (i = 0; i < c->props.num_sources; i++) {
        if (c->sources[i].local) {
            mapper_router_signal rs = c->sources[i].local;
            for (j = 0; j < rs->num_slots; j++) {
                if (rs->slots[j] == &c->sources[i]) {
                    rs->slots[j] = 0;
                }
            }
        }
        if (c->status >= MAPPER_READY && c->sources[i].link) {
            c->sources[i].link->props.num_connections_in--;
            check_link(r, c->sources[i].link);
        }
        free_slot_memory(&c->sources[i]);
    }
    free(c->sources);
    free(c->props.sources);

    // free buffers associated with user-defined expression variables
    if (c->num_expr_vars) {
        for (i = 0; i < c->num_var_instances; i++) {
            if (c->num_expr_vars) {
                for (j = 0; j < c->num_expr_vars; j++) {
                    free(c->expr_vars[i][j].value);
                    free(c->expr_vars[i][j].timetag);
                }
            }
            free(c->expr_vars[i]);
        }
    }
    if (c->expr_vars)
        free(c->expr_vars);
    if (c->expr)
        mapper_expr_free(c->expr);

    if (c->props.expression)
        free(c->props.expression);
    for (i=0; i<c->props.scope.size; i++) {
        free(c->props.scope.names[i]);
    }
    free(c->props.scope.names);
    free(c->props.scope.hashes);
    table_free(c->props.extra, 1);

    free(c);
    return 0;
}

static int match_slot(mapper_device md, mapper_connection_slot slot,
                      const char *full_name)
{
    if (!full_name)
        return 1;
    full_name += (full_name[0]=='/');
    const char *sig_name = strchr(full_name+1, '/');
    if (!sig_name)
        return 1;
    int len = sig_name - full_name;
    const char *local_devname = slot->local ? mdev_name(md) : slot->link->props.name;

    // first compare device name
    if (strlen(local_devname) != len || strncmp(full_name, local_devname, len))
        return 1;

    if (strcmp(sig_name+1, slot->props->signal->name) == 0)
        return 0;
    return 1;
}

mapper_connection mapper_router_find_outgoing_connection(mapper_router router,
                                                         mapper_signal local_src,
                                                         int num_sources,
                                                         const char **src_names,
                                                         const char *dest_name)
{
    // find associated router_signal
    mapper_router_signal rs = router->signals;
    while (rs && rs->signal != local_src)
        rs = rs->next;
    if (!rs)
        return 0;

    // find associated connection
    int i, j;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i] || rs->slots[i]->props->direction == DI_INCOMING)
            continue;
        mapper_connection_slot s = rs->slots[i];
        mapper_connection c = s->connection;

        // check destination
        if (match_slot(router->device, &c->destination, dest_name))
            continue;

        // check sources
        int found = 1;
        for (j = 0; j < c->props.num_sources; j++) {
            if (c->sources[j].local == rs)
                continue;

            if (match_slot(router->device, &c->sources[j], src_names[j])) {
                found = 0;
                break;
            }
        }
        if (found)
            return c;
    }
    return 0;
}

mapper_connection mapper_router_find_incoming_connection(mapper_router router,
                                                         mapper_signal local_dest,
                                                         int num_sources,
                                                         const char **src_names)
{
    // find associated router_signal
    mapper_router_signal rs = router->signals;
    while (rs && rs->signal != local_dest)
        rs = rs->next;
    if (!rs)
        return 0;

    // find associated connection
    int i, j;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i] || rs->slots[i]->props->direction == DI_OUTGOING)
            continue;
        mapper_connection c = rs->slots[i]->connection;

        // check sources
        int found = 1;
        for (j = 0; j < num_sources; j++) {
            if (match_slot(router->device, &c->sources[j], src_names[j])) {
                found = 0;
                break;
            }
        }
        if (found)
            return c;
    }
    return 0;
}

mapper_connection mapper_router_find_incoming_connection_id(mapper_router router,
                                                            mapper_signal local_dest,
                                                            int id)
{
    mapper_router_signal rs = router->signals;
    while (rs && rs->signal != local_dest)
        rs = rs->next;
    if (!rs)
        return 0;

    int i;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i] || rs->slots[i]->props->direction == DI_OUTGOING)
            continue;
        mapper_connection c = rs->slots[i]->connection;
        if (c->props.id == id)
            return c;
    }
    return 0;
}

mapper_connection mapper_router_find_outgoing_connection_id(mapper_router router,
                                                            mapper_signal local_src,
                                                            const char *dest_name,
                                                            int id)
{
    int i;
    mapper_router_signal rs = router->signals;
    while (rs && rs->signal != local_src)
        rs = rs->next;
    if (!rs)
        return 0;

    char *devname, *signame;
    int devnamelen = mapper_parse_names(dest_name, &devname, &signame);
    if (!devnamelen || devnamelen >= 256)
        return 0;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i] || rs->slots[i]->props->direction == DI_INCOMING)
            continue;
        mapper_connection c = rs->slots[i]->connection;
        if (c->props.id != id)
            continue;
        const char *match = (c->destination.local
                             ? mdev_name(router->device)
                             : c->destination.link->props.name);
        if (strlen(match)==devnamelen
            && strncmp(match, devname, devnamelen)==0) {
            return c;
        }
    }
    return 0;
}

mapper_connection_slot mapper_router_find_connection_slot(mapper_router router,
                                                          mapper_signal signal,
                                                          int slot_id)
{
    // only interested in incoming slots
    mapper_router_signal rs = router->signals;
    while (rs && rs->signal != signal)
        rs = rs->next;
    if (!rs)
        return NULL; // no associated router_signal

    int i, j;
    mapper_connection c;
    for (i = 0; i < rs->num_slots; i++) {
        if (!rs->slots[i] || rs->slots[i]->props->direction == DI_OUTGOING)
            continue;
        c = rs->slots[i]->connection;
        // check incoming slots for this connection
        for (j = 0; j < c->props.num_sources; j++) {
            if (c->sources[j].props->slot_id == slot_id)
                return &c->sources[j];
        }
    }
    return NULL;
}

mapper_link mapper_router_find_link_by_remote_address(mapper_router r,
                                                      const char *host,
                                                      int port)
{
    mapper_link l = r->links;
    while (l) {
        if (l->props.port == port && (strcmp(l->props.host, host)==0))
            return l;
        l = l->next;
    }
    return 0;
}

mapper_link mapper_router_find_link_by_remote_name(mapper_router router,
                                                   const char *name)
{
    int n = strlen(name);
    const char *slash = strchr(name+1, '/');
    if (slash)
        n = slash - name;

    mapper_link l = router->links;
    while (l) {
        if (strncmp(l->props.name, name, n)==0 && l->props.name[n]==0)
            return l;
        l = l->next;
    }
    return 0;
}

mapper_link mapper_router_find_link_by_remote_hash(mapper_router router,
                                                   uint32_t name_hash)
{
    mapper_link l = router->links;
    while (l) {
        if (name_hash == l->props.name_hash)
            return l;
        l = l->next;
    }
    return 0;
}
