PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

PEERDIR(
    ydb/core/fq/libs/graph_params/proto
)

SRCS(
    graph_description.proto
)

EXCLUDE_TAGS(GO_PROTO)

END()
