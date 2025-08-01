PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

EXCLUDE_TAGS(GO_PROTO)

PY_NAMESPACE(ydb.yc.priv.operation)

GRPC()
SRCS(
    operation.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
)

END()

