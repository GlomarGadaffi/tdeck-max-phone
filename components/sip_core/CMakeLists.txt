# Vendored from pocket-dial (MIT, (c) 2022 BarGabriel / GlomarGadaffi).
# Used here only as a SIP message parser/serializer for the client spike.
idf_component_register(
    SRCS
        "SipMessage.cpp"
        "SipSdpMessage.cpp"
        "SipMessageFactory.cpp"
    INCLUDE_DIRS "."
    REQUIRES lwip
)
