import AVFoundation
import Combine
import CoreImage
import UIKit

enum CameraLens: String, CaseIterable, Identifiable {
    case ultraWide = "UW"
    case wide = "1x"
    case tele = "Tele"
    case front = "Front"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .ultraWide: return "Ultra Wide"
        case .wide: return "Main"
        case .tele: return "Tele"
        case .front: return "Front"
        }
    }
}

final class CameraEngine: NSObject, ObservableObject {
    let session = AVCaptureSession()
    private let sessionQueue = DispatchQueue(label: "onlycam.session")
    private let videoQueue = DispatchQueue(label: "onlycam.video")
    private let ciContext = CIContext(options: [.useSoftwareRenderer: false])
    private let videoOutput = AVCaptureVideoDataOutput()
    private var currentDevice: AVCaptureDevice?
    private var videoInput: AVCaptureDeviceInput?

    @Published var availableLenses: [CameraLens] = []
    @Published var selectedLens: CameraLens = .ultraWide
    @Published var isRunning = false
    @Published var torchOn = false
    @Published var zoom: CGFloat = 1
    @Published var minZoom: CGFloat = 1
    @Published var maxZoom: CGFloat = 1
    @Published var status = "Idle"
    @Published var jpegQuality: CGFloat = 0.55

    var onJPEG: ((Data) -> Void)?

    func start() {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            configureAndStart()
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { [weak self] granted in
                DispatchQueue.main.async {
                    if granted {
                        self?.configureAndStart()
                    } else {
                        self?.status = "Camera permission denied"
                    }
                }
            }
        default:
            status = "Enable Camera in Settings"
        }
    }

    func stop() {
        sessionQueue.async {
            if self.session.isRunning {
                self.session.stopRunning()
            }
            DispatchQueue.main.async {
                self.isRunning = false
                self.status = "Stopped"
            }
        }
    }

    func selectLens(_ lens: CameraLens) {
        selectedLens = lens
        sessionQueue.async {
            self.reconfigureInput()
        }
    }

    func setTorch(_ on: Bool) {
        sessionQueue.async {
            guard let device = self.currentDevice, device.hasTorch else { return }
            do {
                try device.lockForConfiguration()
                device.torchMode = on ? .on : .off
                device.unlockForConfiguration()
                DispatchQueue.main.async { self.torchOn = on }
            } catch {
                DispatchQueue.main.async { self.status = "Torch failed" }
            }
        }
    }

    func setZoom(_ factor: CGFloat) {
        sessionQueue.async {
            guard let device = self.currentDevice else { return }
            let clamped = min(max(factor, device.minAvailableVideoZoomFactor), device.maxAvailableVideoZoomFactor)
            do {
                try device.lockForConfiguration()
                device.videoZoomFactor = clamped
                device.unlockForConfiguration()
                DispatchQueue.main.async { self.zoom = clamped }
            } catch {
                DispatchQueue.main.async { self.status = "Zoom failed" }
            }
        }
    }

    private func configureAndStart() {
        sessionQueue.async {
            self.session.beginConfiguration()
            self.session.sessionPreset = .hd1920x1080

            if self.session.outputs.isEmpty {
                self.videoOutput.alwaysDiscardsLateVideoFrames = true
                self.videoOutput.videoSettings = [
                    kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
                ]
                self.videoOutput.setSampleBufferDelegate(self, queue: self.videoQueue)
                if self.session.canAddOutput(self.videoOutput) {
                    self.session.addOutput(self.videoOutput)
                }
            }

            self.session.commitConfiguration()
            self.refreshAvailableLenses()
            self.reconfigureInput()

            if !self.session.isRunning {
                self.session.startRunning()
            }
            DispatchQueue.main.async {
                self.isRunning = true
                self.status = "Camera running"
            }
        }
    }

    private func refreshAvailableLenses() {
        var lenses: [CameraLens] = []
        if Self.device(for: .ultraWide) != nil { lenses.append(.ultraWide) }
        if Self.device(for: .wide) != nil { lenses.append(.wide) }
        if Self.device(for: .tele) != nil { lenses.append(.tele) }
        if Self.device(for: .front) != nil { lenses.append(.front) }
        DispatchQueue.main.async {
            self.availableLenses = lenses
            if !lenses.contains(self.selectedLens), let first = lenses.first {
                self.selectedLens = first
            }
        }
    }

    private func reconfigureInput() {
        session.beginConfiguration()
        if let videoInput {
            session.removeInput(videoInput)
            self.videoInput = nil
        }

        let lens = selectedLens
        guard let device = Self.device(for: lens) else {
            session.commitConfiguration()
            DispatchQueue.main.async { self.status = "Lens not available: \(lens.title)" }
            return
        }

        do {
            let input = try AVCaptureDeviceInput(device: device)
            if session.canAddInput(input) {
                session.addInput(input)
                videoInput = input
                currentDevice = device
                pickBestFormat(for: device)
                DispatchQueue.main.async {
                    self.minZoom = device.minAvailableVideoZoomFactor
                    self.maxZoom = min(device.maxAvailableVideoZoomFactor, 6)
                    self.zoom = device.videoZoomFactor
                    self.torchOn = device.torchMode == .on
                    self.status = "Using \(lens.title)"
                }
            }
        } catch {
            DispatchQueue.main.async { self.status = error.localizedDescription }
        }

        if let connection = videoOutput.connection(with: .video) {
            if connection.isVideoRotationAngleSupported(90) {
                connection.videoRotationAngle = 90
            }
            connection.isVideoMirrored = (lens == .front)
        }

        session.commitConfiguration()
    }

    private func pickBestFormat(for device: AVCaptureDevice) {
        let targetWidth = 1920
        let targetHeight = 1080
        let targetFPS: Double = 30

        var best: AVCaptureDevice.Format?
        var bestScore = Int.max

        for format in device.formats {
            let desc = format.formatDescription
            let dims = CMVideoFormatDescriptionGetDimensions(desc)
            guard dims.width >= targetWidth, dims.height >= targetHeight else { continue }
            let fpsOk = format.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= targetFPS }
            guard fpsOk else { continue }
            let score = abs(Int(dims.width) - targetWidth) + abs(Int(dims.height) - targetHeight)
            if score < bestScore {
                bestScore = score
                best = format
            }
        }

        guard let format = best else { return }
        do {
            try device.lockForConfiguration()
            device.activeFormat = format
            device.activeVideoMinFrameDuration = CMTime(value: 1, timescale: 30)
            device.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: 30)
            if device.isFocusModeSupported(.continuousAutoFocus) {
                device.focusMode = .continuousAutoFocus
            }
            if device.isExposureModeSupported(.continuousAutoExposure) {
                device.exposureMode = .continuousAutoExposure
            }
            device.unlockForConfiguration()
        } catch {
            DispatchQueue.main.async { self.status = "Format failed" }
        }
    }

    static func device(for lens: CameraLens) -> AVCaptureDevice? {
        let types: [AVCaptureDevice.DeviceType]
        let position: AVCaptureDevice.Position
        switch lens {
        case .ultraWide:
            types = [.builtInUltraWideCamera]
            position = .back
        case .wide:
            types = [.builtInWideAngleCamera]
            position = .back
        case .tele:
            types = [.builtInTelephotoCamera]
            position = .back
        case .front:
            types = [.builtInUltraWideCamera, .builtInWideAngleCamera]
            position = .front
        }
        let session = AVCaptureDevice.DiscoverySession(
            deviceTypes: types,
            mediaType: .video,
            position: position
        )
        return session.devices.first
    }
}

extension CameraEngine: AVCaptureVideoDataOutputSampleBufferDelegate {
    func captureOutput(
        _ output: AVCaptureOutput,
        didOutput sampleBuffer: CMSampleBuffer,
        from connection: AVCaptureConnection
    ) {
        guard let onJPEG else { return }
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
        guard let cgImage = ciContext.createCGImage(ciImage, from: ciImage.extent) else { return }
        let image = UIImage(cgImage: cgImage)
        let quality = jpegQuality
        guard let data = image.jpegData(compressionQuality: quality) else { return }
        onJPEG(data)
    }
}
