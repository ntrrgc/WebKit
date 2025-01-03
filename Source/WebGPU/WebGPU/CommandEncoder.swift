/*
 * Copyright (c) 2021-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

import Metal
import WebGPU_Internal

// FIXME: rdar://140819194
private let WGPU_COPY_STRIDE_UNDEFINED = WGPU_COPY_STRIDE_UNDEFINED_

// FIXME: rdar://140819448
private let MTLBlitOptionNone = MTLBlitOptionNone_

public func clearBuffer(
    commandEncoder: WebGPU.CommandEncoder, buffer: WebGPU.Buffer, offset: UInt64, size: inout UInt64
) {
    commandEncoder.clearBuffer(buffer: buffer, offset: offset, size: &size)
}
public func resolveQuerySet(commandEncoder: WebGPU.CommandEncoder, querySet: WebGPU.QuerySet, firstQuery: UInt32, queryCount:UInt32, destination: WebGPU.Buffer, destinationOffset: UInt64)
{
    commandEncoder.resolveQuerySet(querySet, firstQuery: firstQuery, queryCount: queryCount, destination: destination, destinationOffset: destinationOffset)
}

@_expose(Cxx)
public func CommandEncoder_copyBufferToTexture_thunk(commandEncoder: WebGPU.CommandEncoder, source: WGPUImageCopyBuffer, destination: WGPUImageCopyTexture, copySize: WGPUExtent3D){
    commandEncoder.copyBufferToTexture(source: source, destination: destination, copySize: copySize)
}

@_expose(Cxx)
public func CommandEncoder_copyTextureToBuffer_thunk(commandEncoder: WebGPU.CommandEncoder, source: WGPUImageCopyTexture, destination: WGPUImageCopyBuffer, copySize: WGPUExtent3D){
    commandEncoder.copyTextureToBuffer(source: source, destination: destination, copySize: copySize)
}

extension WebGPU.CommandEncoder {
    static func hasValidDimensions(dimension: WGPUTextureDimension, width: UInt, height: UInt, depth: UInt) -> Bool {
        switch (dimension.rawValue) {
            case WGPUTextureDimension_1D.rawValue:
                return width != 0
            case WGPUTextureDimension_2D.rawValue:
                return width != 0 && height != 0
            case WGPUTextureDimension_3D.rawValue:
                return width != 0 && height != 0 && depth != 0
            default:
                return true
        }
        return true
    }

    public func copyTextureToBuffer(source: WGPUImageCopyTexture, destination: WGPUImageCopyBuffer, copySize: WGPUExtent3D) {
        guard source.nextInChain == nil && destination.nextInChain == nil && destination.layout.nextInChain == nil else {
            return
        }

        // https://gpuweb.github.io/gpuweb/#dom-gpucommandencoder-copytexturetobuffer

        guard prepareTheEncoderState() else {
            self.generateInvalidEncoderStateError()
            return;
        }

        let sourceTexture = WebGPU.fromAPI(source.texture);
        let error = self.errorValidatingCopyTextureToBuffer(source, destination, copySize)
        guard error != nil else {
            self.makeInvalid(error)
            return
        }

        let apiDestinationBuffer = WebGPU.fromAPI(destination.buffer)
        sourceTexture.setCommandEncoder(self)
        apiDestinationBuffer.setCommandEncoder(self, false)
        apiDestinationBuffer.indirectBufferInvalidated()
        guard !sourceTexture.isDestroyed() && !apiDestinationBuffer.isDestroyed() else {
            return
        }

        var options = MTLBlitOption(rawValue: MTLBlitOptionNone.rawValue)
        switch (source.aspect.rawValue) {
        case WGPUTextureAspect_All.rawValue:
            break
        case WGPUTextureAspect_StencilOnly.rawValue:
            options = MTLBlitOption.stencilFromDepthStencil
        case WGPUTextureAspect_DepthOnly.rawValue:
            options = MTLBlitOption.depthFromDepthStencil
        case WGPUTextureAspect_Force32.rawValue:
            return
        default:
            return
        }

        let logicalSize = sourceTexture.logicalMiplevelSpecificTextureExtent(source.mipLevel)
        let widthForMetal = logicalSize.width < source.origin.x ? 0 : min(copySize.width, logicalSize.width - source.origin.x)
        let heightForMetal = logicalSize.height < source.origin.y ? 0 : min(copySize.height, logicalSize.height - source.origin.y)
        let depthForMetal = logicalSize.depthOrArrayLayers < source.origin.z ? 0 : min(copySize.depthOrArrayLayers, logicalSize.depthOrArrayLayers - source.origin.z)

        guard let destinationBuffer = apiDestinationBuffer.buffer() else {
            return
        }
        var destinationBytesPerRow = UInt(destination.layout.bytesPerRow)
        if (destinationBytesPerRow == WGPU_COPY_STRIDE_UNDEFINED) {
            destinationBytesPerRow = UInt(destinationBuffer.length)
        }

        let sourceTextureFormat = sourceTexture.format()
        let aspectSpecificFormat = WebGPU.Texture.aspectSpecificFormat(sourceTextureFormat, source.aspect)
        let blockSize = WebGPU.Texture.texelBlockSize(aspectSpecificFormat)
        let textureDimension = sourceTexture.dimension()
        var didOverflow: Bool
        switch (textureDimension.rawValue) {
            case WGPUTextureDimension_1D.rawValue:
                if !blockSize.hasOverflowed() {
                    var product: UInt32 = blockSize.value()
                    (product, didOverflow) = product.multipliedReportingOverflow(by: self.m_device.ptr().limitsCopy().maxTextureDimension1D)
                    if !didOverflow {
                        destinationBytesPerRow = min(destinationBytesPerRow, UInt(product))
                    }
                }
            case WGPUTextureDimension_2D.rawValue, WGPUTextureDimension_3D.rawValue:
                if !blockSize.hasOverflowed() {
                    var product: UInt32 = blockSize.value()
                    (product, didOverflow) = product.multipliedReportingOverflow(by: self.m_device.ptr().limitsCopy().maxTextureDimension2D)
                    if !didOverflow {
                        destinationBytesPerRow = min(destinationBytesPerRow, UInt(product))
                    }
                }
            case WGPUTextureDimension_Force32.rawValue:
                break
            default:
                break
        }

        destinationBytesPerRow = WebGPU_Internal.roundUpToMultipleOfNonPowerOfTwoCheckedUInt32UnsignedLong(blockSize, destinationBytesPerRow)
        if textureDimension.rawValue == WGPUTextureDimension_3D.rawValue && copySize.depthOrArrayLayers <= 1 && copySize.height <= 1 {
            destinationBytesPerRow = 0
        }

        var rowsPerImage = destination.layout.rowsPerImage
        if rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED {
            rowsPerImage = heightForMetal != 0 ? heightForMetal : 1
        }
        var destinationBytesPerImage = UInt(rowsPerImage)
        (destinationBytesPerImage, didOverflow) = destinationBytesPerImage.multipliedReportingOverflow(by: destinationBytesPerRow)
        guard !didOverflow else {
            return
        }

        let maxDestinationBytesPerRow: UInt = textureDimension.rawValue == WGPUTextureDimension_3D.rawValue ? (2048 * blockSize.value()) : destinationBytesPerRow
        if destinationBytesPerRow > maxDestinationBytesPerRow {
            for z in 0..<copySize.depthOrArrayLayers {
                var zPlusOriginZ = z
                (zPlusOriginZ, didOverflow) = zPlusOriginZ.addingReportingOverflow(source.origin.z)
                guard !didOverflow else {
                    return
                }
                var zTimesDestinationBytesPerImage = z
                guard destinationBytesPerImage <= UInt32.max else {
                    return
                }
                (zTimesDestinationBytesPerImage, didOverflow) = zTimesDestinationBytesPerImage.multipliedReportingOverflow(by: UInt32(destinationBytesPerImage))
                guard !didOverflow else {
                    return
                }
                for y in 0..<copySize.height {
                    var yPlusOriginY = source.origin.y
                    (yPlusOriginY, didOverflow) = yPlusOriginY.addingReportingOverflow(y)
                    guard !didOverflow else {
                        return
                    }
                    var yTimesDestinationBytesPerImage = y
                    guard destinationBytesPerImage <= UInt32.max else {
                        return
                    }
                    (yTimesDestinationBytesPerImage, didOverflow) = yTimesDestinationBytesPerImage.multipliedReportingOverflow(by: UInt32(destinationBytesPerImage))
                    guard !didOverflow else {
                        return
                    }
                    let newSource = WGPUImageCopyTexture(
                        nextInChain: nil,
                        texture: source.texture,
                        mipLevel: source.mipLevel,
                        origin: WGPUOrigin3D(x: source.origin.x, y: yPlusOriginY, z: zPlusOriginZ),
                        aspect: source.aspect
                    )
                    var tripleSum = UInt64(destination.layout.offset)
                    (tripleSum, didOverflow) = tripleSum.addingReportingOverflow(UInt64(zTimesDestinationBytesPerImage))
                    guard !didOverflow else {
                        return
                    }
                    (tripleSum, didOverflow) = tripleSum.addingReportingOverflow(UInt64(yTimesDestinationBytesPerImage))
                    guard !didOverflow else {
                        return
                    }
                    let newDestination = WGPUImageCopyBuffer(
                        nextInChain: nil,
                        layout: WGPUTextureDataLayout(
                            nextInChain: nil,
                            offset: tripleSum,
                            bytesPerRow: UInt32(WGPU_COPY_STRIDE_UNDEFINED),
                            rowsPerImage: UInt32(WGPU_COPY_STRIDE_UNDEFINED)
                        ),
                        buffer: destination.buffer
                    )
                    self.copyTextureToBuffer(source: newSource, destination: newDestination, copySize: WGPUExtent3D(
                        width: copySize.width,
                        height: 1,
                        depthOrArrayLayers: 1
                    ))
                }
            }
            return
        }

        guard let blitCommandEncoder = ensureBlitCommandEncoder() else {
            return
        }

        for layer in 0..<copySize.depthOrArrayLayers {
            var originZPlusLayer = UInt(source.origin.z)
            (originZPlusLayer, didOverflow) = originZPlusLayer.addingReportingOverflow(UInt(layer))
            guard !didOverflow else {
                return
            }
            let sourceSlice = sourceTexture.dimension().rawValue == WGPUTextureDimension_3D.rawValue ? 0 : originZPlusLayer
            if !sourceTexture.previouslyCleared(source.mipLevel, UInt32(sourceSlice)) {
                clearTextureIfNeeded(source, sourceSlice)
            }
        }

        guard Self.hasValidDimensions(dimension: sourceTexture.dimension(), width: UInt(widthForMetal), height: UInt(heightForMetal), depth: UInt(depthForMetal)) else {
            return
        }

        guard destinationBuffer.length >= WebGPU.Texture.bytesPerRow(aspectSpecificFormat, widthForMetal, sourceTexture.sampleCount()) else {
            return
        }

        switch (sourceTexture.dimension()) {
            case WGPUTextureDimension_1D:
                // https://developer.apple.com/documentation/metal/mtlblitcommandencoder/1400756-copyfromtexture?language=objc
                // "When you copy to a 1D texture, height and depth must be 1."
                let sourceSize = MTLSizeMake(Int(widthForMetal), 1, 1)
                let sourceOrigin = MTLOriginMake(Int(source.origin.x), 0, 0)
                for layer in 0..<copySize.depthOrArrayLayers {
                    var layerTimesDestinationBytesPerImage = UInt(layer)
                    (layerTimesDestinationBytesPerImage, didOverflow) = layerTimesDestinationBytesPerImage.multipliedReportingOverflow(by: destinationBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var destinationOffset = UInt(destination.layout.offset)
                    (destinationOffset, didOverflow) = destinationOffset.addingReportingOverflow(layerTimesDestinationBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var sourceSlice = UInt(source.origin.z)
                    (sourceSlice, didOverflow) = sourceSlice.addingReportingOverflow(UInt(layer))
                    guard !didOverflow else {
                        return
                    }
                    var widthTimesBlockSize = UInt(widthForMetal)
                    (widthTimesBlockSize, didOverflow) = widthTimesBlockSize.multipliedReportingOverflow(by: blockSize.value())
                    guard !didOverflow else {
                        return
                    }
                    let sum = UInt(destinationOffset) + UInt(widthTimesBlockSize)
                    if sum > destinationBuffer.length {
                        continue
                    }
                    blitCommandEncoder.copy(
                        from: sourceTexture.texture(),
                        sourceSlice: Int(sourceSlice),
                        sourceLevel: Int(source.mipLevel),
                        sourceOrigin: sourceOrigin,
                        sourceSize: sourceSize,
                        to: destinationBuffer,
                        destinationOffset: Int(destinationOffset),
                        destinationBytesPerRow: Int(destinationBytesPerRow),
                        destinationBytesPerImage: Int(destinationBytesPerImage),
                        options: options)
                }
            case WGPUTextureDimension_2D:
                // https://developer.apple.com/documentation/metal/mtlblitcommandencoder/1400756-copyfromtexture?language=objc
                // "When you copy to a 2D texture, depth must be 1."
                let sourceSize = MTLSizeMake(Int(widthForMetal), Int(heightForMetal), 1)
                let sourceOrigin = MTLOriginMake(Int(source.origin.x), Int(source.origin.y), 0)
                for layer in 0..<copySize.depthOrArrayLayers {
                    var layerTimesBytesPerImage = UInt(layer)
                    (layerTimesBytesPerImage, didOverflow) = layerTimesBytesPerImage.multipliedReportingOverflow(by: destinationBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var destinationOffset = UInt(destination.layout.offset)
                    (destinationOffset, didOverflow) = destinationOffset.addingReportingOverflow(layerTimesBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var sourceSlice = UInt(source.origin.z)
                    (sourceSlice, didOverflow) = sourceSlice.addingReportingOverflow(UInt(layer))
                    guard !didOverflow else {
                        return
                    }
                    blitCommandEncoder.copy(
                        from: sourceTexture.texture(),
                        sourceSlice: Int(sourceSlice),
                        sourceLevel: Int(source.mipLevel),
                        sourceOrigin: sourceOrigin,
                        sourceSize: sourceSize,
                        to: destinationBuffer,
                        destinationOffset: Int(destinationOffset),
                        destinationBytesPerRow: Int(destinationBytesPerRow),
                        destinationBytesPerImage: Int(destinationBytesPerImage),
                        options: options)
                }
            case WGPUTextureDimension_3D:
                let sourceSize = MTLSizeMake(Int(widthForMetal), Int(heightForMetal), Int(depthForMetal))
                let sourceOrigin = MTLOriginMake(Int(source.origin.x), Int(source.origin.y), Int(source.origin.z))
                let destinationOffset = UInt(destination.layout.offset)
                blitCommandEncoder.copy(
                    from: sourceTexture.texture(),
                    sourceSlice: 0,
                    sourceLevel: Int(source.mipLevel),
                    sourceOrigin: sourceOrigin,
                    sourceSize: sourceSize,
                    to: destinationBuffer,
                    destinationOffset: Int(destinationOffset),
                    destinationBytesPerRow: Int(destinationBytesPerRow),
                    destinationBytesPerImage: Int(destinationBytesPerImage),
                    options: options)
            case WGPUTextureDimension_Force32:
                return
            default:
                return
        }
    }

    public func copyBufferToTexture(source: WGPUImageCopyBuffer, destination: WGPUImageCopyTexture, copySize: WGPUExtent3D) {
        guard source.nextInChain == nil && source.layout.nextInChain == nil && destination.nextInChain == nil else {
            return
        }
        guard self.prepareTheEncoderState() else {
            self.generateInvalidEncoderStateError()
            return
        }
        let destinationTexture = WebGPU.fromAPI(destination.texture)

        let error = self.errorValidatingCopyBufferToTexture(source, destination, copySize)
        guard error != nil else {
            self.makeInvalid(error)
            return
        }
        let apiBuffer = WebGPU.fromAPI(source.buffer)
        apiBuffer.setCommandEncoder(self, false) 
        destinationTexture.setCommandEncoder(self)
        guard copySize.width != 0 || copySize.height != 0 || copySize.depthOrArrayLayers != 0, !apiBuffer.isDestroyed(), !destinationTexture.isDestroyed() else {
            return
        }
        guard let blitCommandEncoder = self.ensureBlitCommandEncoder() else {
            return 
        }
        var sourceBytesPerRow: UInt = UInt(source.layout.bytesPerRow)
        guard let sourceBuffer = apiBuffer.buffer() else {
            return
        }
        if sourceBytesPerRow == WGPU_COPY_STRIDE_UNDEFINED {
            sourceBytesPerRow = UInt(sourceBuffer.length)
        }
        let aspectSpecificFormat = WebGPU.Texture.aspectSpecificFormat(destinationTexture.format(), destination.aspect)
        let blockSize = WebGPU.Texture.texelBlockSize(aspectSpecificFormat)
        // Interesting that swift imports this.. becase I think it knows how to manage WebGPU.Device
        // It will not import raw pointers it does not know how to manage.
        let device = m_device.ptr()
        switch destinationTexture.dimension() {
            case WGPUTextureDimension_1D:
                if !blockSize.hasOverflowed() {
                    // swift cannot infer .value()'s type
                    let blockSizeValue: UInt32 = blockSize.value()
                    let (result, didOverflow) = blockSizeValue.multipliedReportingOverflow(by: device.limitsCopy().maxTextureDimension1D)
                    if !didOverflow {
                        sourceBytesPerRow = UInt(result)
                    }
                }
            case WGPUTextureDimension_2D, WGPUTextureDimension_3D:
                if !blockSize.hasOverflowed() {
                    // swift cannot infer .value()'s type
                    let blockSizeValue: UInt32 = blockSize.value()
                    let (result, didOverflow) = blockSizeValue.multipliedReportingOverflow(by: device.limitsCopy().maxTextureDimension2D)
                    if !didOverflow {
                        sourceBytesPerRow = UInt(result)
                    }
                }
            case WGPUTextureDimension_Force32:
                break
            default:
                break
        }

        var options: MTLBlitOption = MTLBlitOption(rawValue: MTLBlitOptionNone.rawValue)
        switch destination.aspect {
            case WGPUTextureAspect_StencilOnly:
                options = MTLBlitOption.stencilFromDepthStencil
            case WGPUTextureAspect_DepthOnly:
                options = MTLBlitOption.depthFromDepthStencil
            case WGPUTextureAspect_All:
                break
            case WGPUTextureAspect_Force32:
                return
            default:
                return
        }
        let logicalSize = WebGPU.fromAPI(destination.texture).logicalMiplevelSpecificTextureExtent(destination.mipLevel)
        let widthForMetal = logicalSize.width < destination.origin.x ? 0 : min(copySize.width, logicalSize.width - destination.origin.x)
        let heightForMetal = logicalSize.height < destination.origin.y ? 0 : min(copySize.height, logicalSize.height - destination.origin.y)
        let depthForMetal = logicalSize.depthOrArrayLayers < destination.origin.z ? 0 : min(copySize.depthOrArrayLayers, logicalSize.depthOrArrayLayers - destination.origin.z)
        var rowsPerImage = source.layout.rowsPerImage
        if rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED {
            rowsPerImage = heightForMetal != 0 ? rowsPerImage : 1
        }
        var sourceBytesPerImage: UInt
        var didOverflow: Bool
        (sourceBytesPerImage, didOverflow) = UInt(rowsPerImage).multipliedReportingOverflow(by: sourceBytesPerRow)
        guard !didOverflow else {
            return
        }
        let mtlDestinationTexture = destinationTexture.texture()
        let textureDimension = destinationTexture.dimension()

        let sliceCount: UInt32 = textureDimension.rawValue == WGPUTextureDimension_3D.rawValue ? 1 : copySize.depthOrArrayLayers
        for layer in 0..<sliceCount {
            var originPlusLayer = destination.origin.z
            (originPlusLayer, didOverflow) = originPlusLayer.addingReportingOverflow(layer)
            if didOverflow {
                return
            }
            let destinationSlice = destinationTexture.dimension().rawValue == WGPUTextureDimension_3D.rawValue ? 0 : originPlusLayer
            precondition(mtlDestinationTexture != nil, "mtlDestinationTexture is nil")
            precondition(mtlDestinationTexture!.parent == nil, "mtlDestinationTexture.parentTexture is not nil")
            if WebGPU.Queue.writeWillCompletelyClear(textureDimension, widthForMetal, logicalSize.width, heightForMetal, logicalSize.height, depthForMetal, logicalSize.depthOrArrayLayers) {
                destinationTexture.setPreviouslyCleared(destination.mipLevel, destinationSlice, true)
            } else {
                self.clearTextureIfNeeded(destination, UInt(destinationSlice))
            }
        }
        let maxSourceBytesPerRow: UInt = textureDimension.rawValue == WGPUTextureDimension_3D.rawValue ? (2048 * blockSize.value()) : sourceBytesPerRow
        if textureDimension.rawValue == WGPUTextureDimension_3D.rawValue && copySize.depthOrArrayLayers <= 1 && copySize.height <= 1 {
            sourceBytesPerRow = 0
        }
        if sourceBytesPerRow > maxSourceBytesPerRow {
            for z in 0..<copySize.depthOrArrayLayers {
                var destinationOriginPlusZ = destination.origin.z
                (destinationOriginPlusZ, didOverflow) = destinationOriginPlusZ.addingReportingOverflow(z)
                guard !didOverflow else {
                    return
                }
                var zTimesSourceBytesPerImage = z
                guard let sourceBytesPerRowU32 = UInt32(exactly: sourceBytesPerRow) else {
                    return
                }
                (zTimesSourceBytesPerImage, didOverflow) = zTimesSourceBytesPerImage.multipliedReportingOverflow(by: sourceBytesPerRowU32)
                guard !didOverflow else {
                    return
                }
                for y in 0..<copySize.height {
                    var yTimesSourceBytesPerImage = y
                    (yTimesSourceBytesPerImage, didOverflow) = yTimesSourceBytesPerImage.multipliedReportingOverflow(by: sourceBytesPerRowU32)
                    guard !didOverflow else {
                        return
                    }
                    var tripleSum = UInt64(zTimesSourceBytesPerImage)
                    (tripleSum, didOverflow) = tripleSum.addingReportingOverflow(UInt64(yTimesSourceBytesPerImage))
                    guard !didOverflow else {
                        return
                    }
                    (tripleSum, didOverflow) = tripleSum.addingReportingOverflow(UInt64(source.layout.offset))
                    guard !didOverflow else {
                        return
                    }
                    let newSource = WGPUImageCopyBuffer(
                        nextInChain: nil,
                        layout: WGPUTextureDataLayout(
                            nextInChain: nil,
                            offset: tripleSum,
                            bytesPerRow: UInt32(WGPU_COPY_STRIDE_UNDEFINED),
                            rowsPerImage: UInt32(WGPU_COPY_STRIDE_UNDEFINED)
                        ),
                        buffer: source.buffer)
                    var destinationOriginPlusY = y
                    (destinationOriginPlusY, didOverflow) = destinationOriginPlusY.addingReportingOverflow(destination.origin.y)
                    guard !didOverflow else {
                        return
                    }
                    let newDestination = WGPUImageCopyTexture(
                        nextInChain: nil,
                        texture: destination.texture,
                        mipLevel: destination.mipLevel,
                        origin: WGPUOrigin3D(
                            x: destination.origin.x,
                            y: destinationOriginPlusY,
                            z: destinationOriginPlusZ),
                        aspect: destination.aspect
                    )
                    self.copyBufferToTexture(source: newSource, destination: newDestination, copySize: WGPUExtent3D(width: copySize.width, height: 1, depthOrArrayLayers: 1))
                }
            }
            return
        }
        guard sourceBuffer.length >= WebGPU.Texture.bytesPerRow(aspectSpecificFormat, widthForMetal, destinationTexture.sampleCount()) else {
            return
        }
        switch destinationTexture.dimension() {
            case WGPUTextureDimension_1D:
                // https://developer.apple.com/documentation/metal/mtlblitcommandencoder/1400771-copyfrombuffer?language=objc
                // "When you copy to a 1D texture, height and depth must be 1."
                let sourceSize = MTLSizeMake(Int(widthForMetal), 1, 1);
                guard widthForMetal != 0 else {
                    return
                }

                let destinationOrigin = MTLOriginMake(Int(destination.origin.x), 0, 0);
                var widthTimesBlockSize: UInt32 = widthForMetal
                (widthTimesBlockSize, didOverflow) = widthTimesBlockSize.multipliedReportingOverflow(by: blockSize.value())
                guard !didOverflow else {
                    return
                }
                sourceBytesPerRow = min(sourceBytesPerRow, UInt(widthTimesBlockSize));
                for layer in 0..<copySize.depthOrArrayLayers {
                    var layerTimesSourceBytesPerImage = UInt(layer)
                    (layerTimesSourceBytesPerImage, didOverflow) = layerTimesSourceBytesPerImage.multipliedReportingOverflow(by: sourceBytesPerImage)
                    guard !didOverflow else {
                        return
                    }

                    var sourceOffset = UInt(source.layout.offset)
                    (sourceOffset, didOverflow) = sourceOffset.addingReportingOverflow(layerTimesSourceBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var destinationSlice = UInt(destination.origin.z)
                    (destinationSlice, didOverflow) = destinationSlice.addingReportingOverflow(UInt(layer))
                    guard !didOverflow else {
                        return
                    }
                    var sourceOffsetPlusSourceBytesPerRow = sourceOffset
                    (sourceOffsetPlusSourceBytesPerRow, didOverflow) = sourceOffsetPlusSourceBytesPerRow.addingReportingOverflow(sourceBytesPerRow)
                    guard !didOverflow else {
                        return
                    }
                    guard sourceOffsetPlusSourceBytesPerRow <= sourceBuffer.length else {
                        return
                    }
                    blitCommandEncoder.copy(
                        from: sourceBuffer,
                        sourceOffset: Int(sourceOffset),
                        sourceBytesPerRow: Int(sourceBytesPerRow),
                        sourceBytesPerImage: Int(sourceBytesPerImage),
                        sourceSize: sourceSize,
                        to: mtlDestinationTexture!,
                        destinationSlice: Int(destinationSlice),
                        destinationLevel: Int(destination.mipLevel),
                        destinationOrigin: destinationOrigin,
                        options: options
                    )

                }
            case WGPUTextureDimension_2D:
                // https://developer.apple.com/documentation/metal/mtlblitcommandencoder/1400771-copyfrombuffer?language=objc
                // "When you copy to a 2D texture, depth must be 1."
                let sourceSize = MTLSizeMake(Int(widthForMetal), Int(heightForMetal), 1);
                guard widthForMetal != 0 && heightForMetal != 0 else {
                    return
                }

                let destinationOrigin = MTLOriginMake(Int(destination.origin.x), Int(destination.origin.y), 0);
                for layer in 0..<copySize.depthOrArrayLayers {
                    var layerTimesSourceBytesPerImage = UInt(layer)
                    (layerTimesSourceBytesPerImage, didOverflow) = layerTimesSourceBytesPerImage.addingReportingOverflow(sourceBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var sourceOffset = UInt(source.layout.offset)
                    (sourceOffset, didOverflow) = sourceOffset.addingReportingOverflow(layerTimesSourceBytesPerImage)
                    guard !didOverflow else {
                        return
                    }
                    var destinationSlice = UInt(destination.origin.z)
                    (destinationSlice, didOverflow) = destinationSlice.addingReportingOverflow(UInt(layer))
                    guard !didOverflow else {
                        return
                    }
                    blitCommandEncoder.copy(
                        from: sourceBuffer,
                        sourceOffset: Int(sourceOffset),
                        sourceBytesPerRow: Int(sourceBytesPerRow),
                        sourceBytesPerImage: Int(sourceBytesPerImage),
                        sourceSize: sourceSize,
                        to: mtlDestinationTexture!,
                        destinationSlice: Int(destinationSlice),
                        destinationLevel: Int(destination.mipLevel),
                        destinationOrigin: destinationOrigin,
                        options: options
                    )

                }

            case WGPUTextureDimension_3D:
                let sourceSize = MTLSizeMake(Int(widthForMetal), Int(heightForMetal), Int(depthForMetal));
                guard widthForMetal != 0 && heightForMetal != 0 && depthForMetal != 0 else {
                    return
                }

                let destinationOrigin = MTLOriginMake(Int(destination.origin.x), Int(destination.origin.y), Int(destination.origin.z))
                let sourceOffset = UInt(source.layout.offset)
                blitCommandEncoder.copy(
                    from: sourceBuffer,
                    sourceOffset: Int(sourceOffset),
                    sourceBytesPerRow: Int(sourceBytesPerRow),
                    sourceBytesPerImage: Int(sourceBytesPerImage),
                    sourceSize: sourceSize,
                    to: mtlDestinationTexture!,
                    destinationSlice: 0,
                    destinationLevel: Int(destination.mipLevel),
                    destinationOrigin: destinationOrigin,
                    options: options
                )
            case WGPUTextureDimension_Force32:
                assertionFailure("ASSERT_NOT_REACHED")
                return
            default:
                assertionFailure("ASSERT_NOT_REACHED")
                return
        }

    }

    public func clearBuffer(buffer: WebGPU.Buffer, offset: UInt64, size: inout UInt64) {
        guard self.prepareTheEncoderState() else {
            self.generateInvalidEncoderStateError()
            return
        }
        if size == UInt64.max {
            let initialSize = buffer.initialSize()
            let (subtractionResult, didOverflow) = initialSize.subtractingReportingOverflow(offset)
            if didOverflow {
                self.device().generateAValidationError(
                    "CommandEncoder::clearBuffer(): offset > buffer.size")
                return
            }
            size = subtractionResult
        }

        if !self.validateClearBuffer(buffer, offset, size) {
            self.makeInvalid("GPUCommandEncoder.clearBuffer validation failed")
            return
        }
        // FIXME: rdar://138042799 need to pass in the default argument.
        buffer.setCommandEncoder(self, false)
        buffer.indirectBufferInvalidated()
        guard let offsetInt = Int(exactly: offset), let sizeInt = Int(exactly: size) else {
            return
        }
        let range = offsetInt..<(offsetInt + sizeInt)
        if buffer.isDestroyed() || sizeInt == 0 || range.upperBound > buffer.buffer().length {
            return
        }
        guard let blitCommandEncoder = ensureBlitCommandEncoder() else {
            return
        }
        blitCommandEncoder.fill(buffer: buffer.buffer(), range: range, value: 0)
    }

    public func resolveQuerySet(_ querySet: WebGPU.QuerySet, firstQuery: UInt32, queryCount: UInt32, destination: WebGPU.Buffer, destinationOffset:UInt64)
    {
        guard self.prepareTheEncoderState() else {
            self.generateInvalidEncoderStateError()
            return
        }
        guard self.validateResolveQuerySet(querySet: querySet, firstQuery: firstQuery, queryCount: queryCount, destination: destination, destinationOffset: destinationOffset) else {
            self.makeInvalid("GPUCommandEncoder.resolveQuerySet validation failed")
            return
        }
        querySet.setCommandEncoder(self)
        // FIXME: rdar://138042799 need to pass in the default argument.
        destination.setCommandEncoder(self, false)
        destination.indirectBufferInvalidated();
        guard !(querySet.isDestroyed() || destination.isDestroyed() || queryCount == 0) else {
            return
        }
        guard let blitCommandEncoder = ensureBlitCommandEncoder() else {
            return
        }
        if querySet.type().rawValue == WGPUQueryType_Occlusion.rawValue {
            guard let sourceOffset = Int(exactly: 8 * firstQuery), let destinationOffsetChecked = Int(exactly: destinationOffset), let size = Int(exactly: 8 * queryCount) else {
                return
            }
            blitCommandEncoder.copy(from: querySet.visibilityBuffer(), sourceOffset: sourceOffset, to: destination.buffer(), destinationOffset: destinationOffsetChecked, size: size)
        }
    }
    public func validateResolveQuerySet(querySet: WebGPU.QuerySet, firstQuery: UInt32, queryCount: UInt32, destination: WebGPU.Buffer, destinationOffset: UInt64) -> Bool
    {
        guard (destinationOffset % 256) == 0 else {
            return false
        }
        guard querySet.isDestroyed() || querySet.isValid() else {
            return false
        }
        guard destination.isDestroyed() || destination.isValid() else {
            return false
        }
        guard (destination.usage() & WGPUBufferUsage_QueryResolve.rawValue) != 0 else {
            return false
        }
        guard firstQuery < querySet.count() else {
            return false
        }
        let countEnd: UInt32 = firstQuery
        var (additionResult, didOverflow) = countEnd.addingReportingOverflow(queryCount)
        guard !didOverflow && additionResult <= querySet.count() else {
            return false
        }
        let countTimes8PlusOffset = destinationOffset
        let additionResult64: UInt64
        (additionResult64, didOverflow) = countTimes8PlusOffset.addingReportingOverflow(8 * UInt64(queryCount))
        guard !didOverflow && destination.initialSize() >= additionResult64 else {
            return false
        }
        return true
    }
}
