/* replication_cursor_type.c - python interface to replication cursor objects
 *
 * Copyright (C) 2015 Daniele Varrazzo <daniele.varrazzo@gmail.com>
 *
 * This file is part of psycopg.
 *
 * psycopg2 is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link this program with the OpenSSL library (or with
 * modified versions of OpenSSL that use the same license as OpenSSL),
 * and distribute linked combinations including the two.
 *
 * You must obey the GNU Lesser General Public License in all respects for
 * all of the code used other than OpenSSL.
 *
 * psycopg2 is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 */

#define PSYCOPG_MODULE
#include "psycopg/psycopg.h"

#include "psycopg/replication_cursor.h"
#include "psycopg/replication_message.h"
#include "psycopg/green.h"
#include "psycopg/pqpath.h"

#include <string.h>
#include <stdlib.h>

/* python */
#include "datetime.h"


#define psyco_repl_curs_start_replication_expert_doc                                \
"start_replication_expert(command, writer=None, keepalive_interval=10) -- Start replication stream with a directly given command."

static PyObject *
psyco_repl_curs_start_replication_expert(replicationCursorObject *self,
                                         PyObject *args, PyObject *kwargs)
{
    cursorObject *curs = &self->cur;
    connectionObject *conn = self->cur.conn;
    PyObject *res = NULL;
    char *command;
    static char *kwlist[] = {"command", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s", kwlist, &command)) {
        return NULL;
    }

    EXC_IF_CURS_CLOSED(curs);
    EXC_IF_GREEN(start_replication_expert);
    EXC_IF_TPC_PREPARED(conn, start_replication_expert);
    EXC_IF_REPLICATING(self, start_replication_expert);

    Dprintf("psyco_repl_curs_start_replication_expert: %s", command);

    /*    self->copysize = 0;*/

    gettimeofday(&self->last_io, NULL);

    if (pq_execute(curs, command, conn->async, 1 /* no_result */, 1 /* no_begin */) >= 0) {
        res = Py_None;
        Py_INCREF(res);

        self->started = 1;
    }

    return res;
}

#define psyco_repl_curs_consume_stream_doc \
"consume_stream(consumer, keepalive_interval=10) -- Consume replication stream."

static PyObject *
psyco_repl_curs_consume_stream(replicationCursorObject *self,
                               PyObject *args, PyObject *kwargs)
{
    cursorObject *curs = &self->cur;
    PyObject *consume = NULL, *res = NULL;
    int decode = 0;
    double keepalive_interval = 10;
    static char *kwlist[] = {"consume", "decode", "keepalive_interval", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|id", kwlist,
                                     &consume, &decode, &keepalive_interval)) {
        return NULL;
    }

    EXC_IF_CURS_CLOSED(curs);
    EXC_IF_CURS_ASYNC(curs, consume_stream);
    EXC_IF_GREEN(consume_stream);
    EXC_IF_TPC_PREPARED(self->cur.conn, consume_stream);
    EXC_IF_NOT_REPLICATING(self, consume_stream);

    if (self->consuming) {
        PyErr_SetString(ProgrammingError,
                        "consume_stream cannot be used when already in the consume loop");
        return NULL;
    }

    Dprintf("psyco_repl_curs_consume_stream");

    if (keepalive_interval < 1.0) {
        psyco_set_error(ProgrammingError, curs, "keepalive_interval must be >= 1 (sec)");
        return NULL;
    }

    self->consuming = 1;

    if (pq_copy_both(self, consume, decode, keepalive_interval) >= 0) {
        res = Py_None;
        Py_INCREF(res);
    }

    self->consuming = 0;

    return res;
}

#define psyco_repl_curs_read_message_doc \
"read_message(decode=True) -- Try reading a replication message from the server (non-blocking)."

static PyObject *
psyco_repl_curs_read_message(replicationCursorObject *self,
                             PyObject *args, PyObject *kwargs)
{
    cursorObject *curs = &self->cur;
    int decode = 1;
    static char *kwlist[] = {"decode", NULL};

    EXC_IF_CURS_CLOSED(curs);
    EXC_IF_GREEN(read_message);
    EXC_IF_TPC_PREPARED(self->cur.conn, read_message);
    EXC_IF_NOT_REPLICATING(self, read_message);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist,
                                     &decode)) {
        return NULL;
    }

    return pq_read_replication_message(self, decode);
}

static PyObject *
repl_curs_flush_feedback(replicationCursorObject *self, int reply)
{
    if (!(self->feedback_pending || reply))
        Py_RETURN_TRUE;

    if (pq_send_replication_feedback(self, reply)) {
        self->feedback_pending = 0;
        Py_RETURN_TRUE;
    } else {
        self->feedback_pending = 1;
        Py_RETURN_FALSE;
    }
}

#define psyco_repl_curs_send_feedback_doc \
"send_feedback(write_lsn=0, flush_lsn=0, apply_lsn=0, reply=False) -- Try sending a replication feedback message to the server and optionally request a reply."

static PyObject *
psyco_repl_curs_send_feedback(replicationCursorObject *self,
                              PyObject *args, PyObject *kwargs)
{
    cursorObject *curs = &self->cur;
    XLogRecPtr write_lsn = InvalidXLogRecPtr,
               flush_lsn = InvalidXLogRecPtr,
               apply_lsn = InvalidXLogRecPtr;
    int reply = 0;
    static char* kwlist[] = {"write_lsn", "flush_lsn", "apply_lsn", "reply", NULL};

    EXC_IF_CURS_CLOSED(curs);
    EXC_IF_NOT_REPLICATING(self, send_feedback);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|KKKi", kwlist,
                                     &write_lsn, &flush_lsn, &apply_lsn, &reply)) {
        return NULL;
    }

    if (write_lsn > self->write_lsn)
        self->write_lsn = write_lsn;

    if (flush_lsn > self->flush_lsn)
        self->flush_lsn = flush_lsn;

    if (apply_lsn > self->apply_lsn)
        self->apply_lsn = apply_lsn;

    self->feedback_pending = 1;

    return repl_curs_flush_feedback(self, reply);
}

#define psyco_repl_curs_flush_feedback_doc \
"flush_feedback(reply=False) -- Try flushing the latest pending replication feedback message to the server and optionally request a reply."

static PyObject *
psyco_repl_curs_flush_feedback(replicationCursorObject *self,
                               PyObject *args, PyObject *kwargs)
{
    cursorObject *curs = &self->cur;
    int reply = 0;
    static char *kwlist[] = {"reply", NULL};

    EXC_IF_CURS_CLOSED(curs);
    EXC_IF_NOT_REPLICATING(self, flush_feedback);

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist,
                                     &reply)) {
        return NULL;
    }

    return repl_curs_flush_feedback(self, reply);
}


RAISES_NEG int
psyco_repl_curs_datetime_init(void)
{
    Dprintf("psyco_repl_curs_datetime_init: datetime init");

    PyDateTime_IMPORT;

    if (!PyDateTimeAPI) {
        PyErr_SetString(PyExc_ImportError, "datetime initialization failed");
        return -1;
    }
    return 0;
}

#define psyco_repl_curs_io_timestamp_doc \
"io_timestamp -- the timestamp of latest IO with the server"

static PyObject *
psyco_repl_curs_get_io_timestamp(replicationCursorObject *self)
{
    cursorObject *curs = &self->cur;
    PyObject *tval, *res = NULL;
    double seconds;

    EXC_IF_CURS_CLOSED(curs);

    seconds = self->last_io.tv_sec + self->last_io.tv_usec / 1.0e6;

    tval = Py_BuildValue("(d)", seconds);
    if (tval) {
        res = PyDateTime_FromTimestamp(tval);
        Py_DECREF(tval);
    }
    return res;
}

/* object method list */

static struct PyMethodDef replicationCursorObject_methods[] = {
    {"start_replication_expert", (PyCFunction)psyco_repl_curs_start_replication_expert,
     METH_VARARGS|METH_KEYWORDS, psyco_repl_curs_start_replication_expert_doc},
    {"consume_stream", (PyCFunction)psyco_repl_curs_consume_stream,
     METH_VARARGS|METH_KEYWORDS, psyco_repl_curs_consume_stream_doc},
    {"read_message", (PyCFunction)psyco_repl_curs_read_message,
     METH_VARARGS|METH_KEYWORDS, psyco_repl_curs_read_message_doc},
    {"send_feedback", (PyCFunction)psyco_repl_curs_send_feedback,
     METH_VARARGS|METH_KEYWORDS, psyco_repl_curs_send_feedback_doc},
    {"flush_feedback", (PyCFunction)psyco_repl_curs_flush_feedback,
     METH_VARARGS|METH_KEYWORDS, psyco_repl_curs_flush_feedback_doc},
    {NULL}
};

/* object calculated member list */

static struct PyGetSetDef replicationCursorObject_getsets[] = {
    { "io_timestamp",
      (getter)psyco_repl_curs_get_io_timestamp, NULL,
      psyco_repl_curs_io_timestamp_doc, NULL },
    {NULL}
};

static int
replicationCursor_setup(replicationCursorObject* self)
{
    self->started = 0;
    self->consuming = 0;

    self->write_lsn = InvalidXLogRecPtr;
    self->flush_lsn = InvalidXLogRecPtr;
    self->apply_lsn = InvalidXLogRecPtr;
    self->feedback_pending = 0;

    return 0;
}

static int
replicationCursor_init(PyObject *obj, PyObject *args, PyObject *kwargs)
{
    replicationCursor_setup((replicationCursorObject *)obj);
    return cursor_init(obj, args, kwargs);
}

static PyObject *
replicationCursor_repr(replicationCursorObject *self)
{
    return PyString_FromFormat(
        "<ReplicationCursor object at %p; closed: %d>", self, self->cur.closed);
}


/* object type */

#define replicationCursorType_doc \
"A database replication cursor."

PyTypeObject replicationCursorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "psycopg2.extensions.ReplicationCursor",
    sizeof(replicationCursorObject), 0,
    0,          /*tp_dealloc*/
    0,          /*tp_print*/
    0,          /*tp_getattr*/
    0,          /*tp_setattr*/
    0,          /*tp_compare*/
    (reprfunc)replicationCursor_repr, /*tp_repr*/
    0,          /*tp_as_number*/
    0,          /*tp_as_sequence*/
    0,          /*tp_as_mapping*/
    0,          /*tp_hash*/
    0,          /*tp_call*/
    (reprfunc)replicationCursor_repr, /*tp_str*/
    0,          /*tp_getattro*/
    0,          /*tp_setattro*/
    0,          /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_ITER |
      Py_TPFLAGS_HAVE_GC, /*tp_flags*/
    replicationCursorType_doc, /*tp_doc*/
    0,          /*tp_traverse*/
    0,          /*tp_clear*/
    0,          /*tp_richcompare*/
    0,          /*tp_weaklistoffset*/
    0,          /*tp_iter*/
    0,          /*tp_iternext*/
    replicationCursorObject_methods, /*tp_methods*/
    0,          /*tp_members*/
    replicationCursorObject_getsets, /*tp_getset*/
    &cursorType, /*tp_base*/
    0,          /*tp_dict*/
    0,          /*tp_descr_get*/
    0,          /*tp_descr_set*/
    0,          /*tp_dictoffset*/
    replicationCursor_init, /*tp_init*/
    0,          /*tp_alloc*/
    0,          /*tp_new*/
};
