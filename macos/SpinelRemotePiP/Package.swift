// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "SpinelRemotePiP",
    platforms: [
        .macOS(.v14)
    ],
    products: [
        .library(name: "SpinelRemoteCore", targets: ["SpinelRemoteCore"]),
        .executable(name: "SpinelRemotePiP", targets: ["SpinelRemotePiP"]),
        .executable(name: "SpinelRemoteSelfTest", targets: ["SpinelRemoteSelfTest"])
    ],
    targets: [
        .target(
            name: "SpinelRemoteCore",
            linkerSettings: [
                .linkedLibrary("z"),
                .linkedFramework("Network"),
                .linkedFramework("Security"),
                .linkedFramework("CryptoKit")
            ]
        ),
        .executableTarget(
            name: "SpinelRemotePiP",
            dependencies: ["SpinelRemoteCore"],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("SwiftUI")
            ]
        ),
        .executableTarget(
            name: "SpinelRemoteSelfTest",
            dependencies: ["SpinelRemoteCore"]
        )
    ]
)
