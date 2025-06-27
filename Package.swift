// swift-tools-version:6.0

import PackageDescription

let package = Package(
    name: "TPCircularBuffer",
    products: [
        .library(
            name: "TPCircularBuffer",
            targets: ["CxxTPCircularBuffer"]
        ),
    ],
    targets: [
        .target(
            name: "CxxTPCircularBuffer",
            swiftSettings: [.interoperabilityMode(.Cxx)]
        ),
    ]
)
