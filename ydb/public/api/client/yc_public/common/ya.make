PROTO_LIBRARY()
PROTOC_FATAL_WARNINGS()

EXCLUDE_TAGS(GO_PROTO)

GRPC()
SRCS(
    validation.proto
)

USE_COMMON_GOOGLE_APIS(
    api/annotations
)

END()

