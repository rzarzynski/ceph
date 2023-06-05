
Serialization (encode/decode)
=============================

When a structure is sent over the network or written to disk, it is
encoded into a string of bytes. Usually (but not always -- mulitple
serialization facilities coexist in Ceph) serializable structures
have ``encode`` and ``decode`` methods that write and read from
``bufferlist`` objects representing byte strings.

Terminology
-----------
It is useful to think not in the domain of daemons and clients but
encoders and decoders. An encoder serializes a structure into bufferlist
while decoder does the opposite.

Encoders and decoders are sometimes refferred collectively as
dencoders.

Dencoders (both encoders and docoders) live within daemons and clients.
For instance, when an RBD client issues an IO operation, it prepares
an instance of the ``MOSDOp`` structure and encodes it into a bufferlist
that is put on the wire.
An OSD reads these bytes and decodes it back to an ``MOSDOp`` instance.
Here encoder was used by the client while decoder by the OSD. However,
these roles can quickly swap -- just imagine handling of the response:
OSD encodes the ``MOSDOpReply`` while the RBD clients decodes it.
Hence, clients and daemons use encoders as decoders as well.

Encoder and decoder operate accordingly to a format which is defined
by a programmer implementing the ``encode`` and ``decode`` methods.

Principles for format change
----------------------------
It is not unusual that the format of serializtion changes. This
process requires careful attention from developers as well as
reviewers.

The general rule is that a decoder must understand what had been
encoded by an encoder. Most of the problems comes from ensuring
this stays also between old decoders and new encoders and
new decoders and old decoders. It is because one shall assume
that -- if not otherwise derogated -- any mix (old/new) is
possible in a cluster. There are 2 main reasons for that:

1. Upgrades. Although there are recommendations related to the order
   of enttity types, it is not mandatory and no assumption should
   be made about it.
2. Huge variability of client versions. It was always the case
   that kernel (and thus kernel clients) upgrades are decoupled
   from Ceph upgrades. Moreover, proliferaton of contenerization
   bring the variability even to e.g. ``librbd`` -- now user space
   libraries live on the container own.

With this being said, there are few rules limiting the degree
of interoperability between dencoders:

* n-2 for dencoding between daemons,
* n-3 hard requirements for client-involved scenarios,
* n-3..  soft requirements for clinet-involved scenarios. Ideally
  every client should be able to talk any version of deamons.

Frameworks
----------
* encoding.h
* denc.h,
* the `Message` hierarchy.

Adding a field to a structure
-----------------------------

You can see examples of this all over the Ceph code, but here's an
example:

.. code-block:: cpp

    class AcmeClass
    {
        int member1;
        std::string member2;

        void encode(bufferlist &bl)
        {
            ENCODE_START(1, 1, bl);
            ::encode(member1, bl);
            ::encode(member2, bl);
            ENCODE_FINISH(bl);
        }

        void decode(bufferlist::iterator &bl)
        {
            DECODE_START(1, bl);
            ::decode(member1, bl);
            ::decode(member2, bl);
            DECODE_FINISH(bl);
        }
    };

The ``ENCODE_START`` macro writes a header that specifies a *version* and
a *compat_version* (both initially 1).  The message version is incremented
whenever a change is made to the encoding.  The compat_version is incremented
only if the change will break existing decoders -- decoders are tolerant
of trailing bytes, so changes that add fields at the end of the structure
do not require incrementing compat_version.

The ``DECODE_START`` macro takes an argument specifying the most recent
message version that the code can handle.  This is compared with the
compat_version encoded in the message, and if the message is too new then
an exception will be thrown.  Because changes to compat_version are rare,
this isn't usually something to worry about when adding fields.

In practice, changes to encoding usually involve simply adding the desired fields
at the end of the ``encode`` and ``decode`` functions, and incrementing
the versions in ``ENCODE_START`` and ``DECODE_START``.  For example, here's how
to add a third field to ``AcmeClass``:

.. code-block:: cpp

    class AcmeClass
    {
        int member1;
        std::string member2;
        std::vector<std::string> member3;

        void encode(bufferlist &bl)
        {
            ENCODE_START(2, 1, bl);
            ::encode(member1, bl);
            ::encode(member2, bl);
            ::encode(member3, bl);
            ENCODE_FINISH(bl);
        }

        void decode(bufferlist::iterator &bl)
        {
            DECODE_START(2, bl);
            ::decode(member1, bl);
            ::decode(member2, bl);
            if (struct_v >= 2) {
                ::decode(member3, bl);
            }
            DECODE_FINISH(bl);
        }
    };

Note that the compat_version did not change because the encoded message
will still be decodable by versions of the code that only understand
version 1 -- they will just ignore the trailing bytes where we encode ``member3``.

In the ``decode`` function, decoding the new field is conditional: this is
because we might still be passed older-versioned messages that do not
have the field.  The ``struct_v`` variable is a local set by the ``DECODE_START``
macro.

# Into the weeeds

The append-extendability of our dencoders is a result of forward
compatibilty between encoders and decoders which is driving factor
behind the ``ENCODE_START`` and ``DECODE_FINISH`` macros.

They are hiding an extensibilty facilities. Encoder, when filling
the bufferlist, adds at the front 3 fields: version of the formating,
minimal version of the compatible decoder and the total size of
all encoded fields.
The size info allows the decoder to eat all the bytes that were left
the user-provided ``decode`` implementations. This allows encoder to
generate more (due to e.g. adding a new field at the end) and not worry
that the residue will trick decoder.
