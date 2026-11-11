import Foundation
import Vision
import AppKit

struct LandmarkPoint: Codable {
    let x: Double
    let y: Double
}

struct FaceData: Codable {
    let bbox: [Double] // [x, y, w, h] in image pixels from top-left
    let is_dehancer_half: Bool
    let landmarks: [String: [LandmarkPoint]]
}

let args = CommandLine.arguments
guard args.count > 1 else {
    print("Usage: detect_faces <image_path>")
    exit(1)
}

let path = args[1]
let url = URL(fileURLWithPath: path)
guard let image = NSImage(contentsOf: url),
      let tiffData = image.tiffRepresentation,
      let ciImage = CIImage(data: tiffData) else {
    print("[]")
    exit(0)
}

let imgW = Double(image.representations[0].pixelsWide)
let imgH = Double(image.representations[0].pixelsHigh)

let handler = VNImageRequestHandler(ciImage: ciImage, options: [:])
let request = VNDetectFaceLandmarksRequest()

do {
    try handler.perform([request])
    guard let results = request.results else {
        print("[]")
        exit(0)
    }
    
    var faces: [FaceData] = []
    
    for face in results {
        // Convert Vision coords (origin bottom-left, normalized) to top-left image pixels
        let bx = face.boundingBox.origin.x * imgW
        let by = (1.0 - face.boundingBox.origin.y - face.boundingBox.size.height) * imgH
        let bw = face.boundingBox.size.width * imgW
        let bh = face.boundingBox.size.height * imgH
        
        let isDeh = (face.boundingBox.origin.x >= 0.45)
        
        var lms: [String: [LandmarkPoint]] = [:]
        if let landmarks = face.landmarks {
            if let contour = landmarks.faceContour {
                lms["faceContour"] = contour.normalizedPoints.map { pt in
                    let px = (face.boundingBox.origin.x + pt.x * face.boundingBox.size.width) * imgW
                    let py = (1.0 - (face.boundingBox.origin.y + pt.y * face.boundingBox.size.height)) * imgH
                    return LandmarkPoint(x: px, y: py)
                }
            }
            if let leftEye = landmarks.leftEye {
                lms["leftEye"] = leftEye.normalizedPoints.map { pt in
                    let px = (face.boundingBox.origin.x + pt.x * face.boundingBox.size.width) * imgW
                    let py = (1.0 - (face.boundingBox.origin.y + pt.y * face.boundingBox.size.height)) * imgH
                    return LandmarkPoint(x: px, y: py)
                }
            }
            if let rightEye = landmarks.rightEye {
                lms["rightEye"] = rightEye.normalizedPoints.map { pt in
                    let px = (face.boundingBox.origin.x + pt.x * face.boundingBox.size.width) * imgW
                    let py = (1.0 - (face.boundingBox.origin.y + pt.y * face.boundingBox.size.height)) * imgH
                    return LandmarkPoint(x: px, y: py)
                }
            }
            if let outerLips = landmarks.outerLips {
                lms["outerLips"] = outerLips.normalizedPoints.map { pt in
                    let px = (face.boundingBox.origin.x + pt.x * face.boundingBox.size.width) * imgW
                    let py = (1.0 - (face.boundingBox.origin.y + pt.y * face.boundingBox.size.height)) * imgH
                    return LandmarkPoint(x: px, y: py)
                }
            }
            if let nose = landmarks.nose {
                lms["nose"] = nose.normalizedPoints.map { pt in
                    let px = (face.boundingBox.origin.x + pt.x * face.boundingBox.size.width) * imgW
                    let py = (1.0 - (face.boundingBox.origin.y + pt.y * face.boundingBox.size.height)) * imgH
                    return LandmarkPoint(x: px, y: py)
                }
            }
        }
        
        faces.append(FaceData(bbox: [bx, by, bw, bh], is_dehancer_half: isDeh, landmarks: lms))
    }
    
    let encoder = JSONEncoder()
    encoder.outputFormatting = .prettyPrinted
    let data = try encoder.encode(faces)
    if let jsonStr = String(data: data, encoding: .utf8) {
        print(jsonStr)
    }
} catch {
    print("[]")
}
