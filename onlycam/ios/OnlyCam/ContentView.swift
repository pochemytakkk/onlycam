import AVFoundation
import Darwin
import SwiftUI
import UIKit

struct ContentView: View {
    @StateObject private var camera = CameraEngine()
    @StateObject private var server = StreamServer()
    @State private var ipAddress = LocalIP.current() ?? "Wi-Fi?"

    var body: some View {
        ZStack {
            CameraPreview(session: camera.session)
                .ignoresSafeArea()

            VStack {
                header
                Spacer()
                controls
            }
            .padding()
        }
        .onAppear {
            UIApplication.shared.isIdleTimerDisabled = true
            camera.start()
            camera.onJPEG = { [weak server] data in
                server?.push(jpeg: data)
            }
            server.start(port: 8080)
            ipAddress = LocalIP.current() ?? "Wi-Fi?"
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
            camera.stop()
            server.stop()
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("OnlyCam")
                .font(.title2.bold())
            Text("\(ipAddress):8080/stream")
                .font(.system(.footnote, design: .monospaced))
            Text(camera.status)
                .font(.caption)
            if let error = server.lastError {
                Text(error).font(.caption2).foregroundStyle(.red)
            } else {
                Text(server.isListening ? "Stream on · clients \(server.clientCount)" : "Stream off")
                    .font(.caption2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(12)
        .background(.black.opacity(0.45), in: RoundedRectangle(cornerRadius: 14))
        .foregroundStyle(.white)
    }

    private var controls: some View {
        VStack(spacing: 12) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack {
                    ForEach(camera.availableLenses) { lens in
                        Button(lens.title) {
                            camera.selectLens(lens)
                        }
                        .buttonStyle(.borderedProminent)
                        .tint(camera.selectedLens == lens ? .orange : .gray.opacity(0.7))
                    }
                }
            }

            HStack {
                Text("Zoom")
                Slider(
                    value: Binding(
                        get: { camera.zoom },
                        set: { camera.setZoom($0) }
                    ),
                    in: camera.minZoom...max(camera.maxZoom, camera.minZoom + 0.01)
                )
            }

            HStack {
                Text("JPEG")
                Slider(value: $camera.jpegQuality, in: 0.3...0.85)
                Button(camera.torchOn ? "Torch On" : "Torch") {
                    camera.setTorch(!camera.torchOn)
                }
                .buttonStyle(.bordered)
            }
        }
        .padding(12)
        .background(.black.opacity(0.5), in: RoundedRectangle(cornerRadius: 16))
        .foregroundStyle(.white)
    }
}

struct CameraPreview: UIViewRepresentable {
    let session: AVCaptureSession

    func makeUIView(context: Context) -> PreviewView {
        let view = PreviewView()
        view.previewLayer.session = session
        view.previewLayer.videoGravity = .resizeAspectFill
        return view
    }

    func updateUIView(_ uiView: PreviewView, context: Context) {
        uiView.previewLayer.session = session
    }
}

final class PreviewView: UIView {
    override class var layerClass: AnyClass { AVCaptureVideoPreviewLayer.self }
    var previewLayer: AVCaptureVideoPreviewLayer { layer as! AVCaptureVideoPreviewLayer }
}

enum LocalIP {
    static func current() -> String? {
        var address: String?
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return nil }
        defer { freeifaddrs(ifaddr) }

        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let interface = ptr.pointee
            guard let ifaAddr = interface.ifa_addr else { continue }
            let family = ifaAddr.pointee.sa_family
            let name = String(cString: interface.ifa_name)
            guard family == sa_family_t(AF_INET), name == "en0" else { continue }
            var hostname = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            getnameinfo(
                ifaAddr,
                socklen_t(ifaAddr.pointee.sa_len),
                &hostname,
                socklen_t(hostname.count),
                nil,
                0,
                NI_NUMERICHOST
            )
            address = String(cString: hostname)
        }
        return address
    }
}
