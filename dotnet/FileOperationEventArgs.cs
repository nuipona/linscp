using System.Runtime.InteropServices;

namespace LinuxSCP
{
    [Guid("FF8D5772-2653-4C9B-870E-4C5EF8F55673")]
    [ClassInterface(Constants.ClassInterface)]
    [ComVisible(true)]
    public class FileOperationEventArgs : OperationEventArgs
    {
        public string FileName { get; internal set; }

        internal FileOperationEventArgs()
        {
        }
    }
}
