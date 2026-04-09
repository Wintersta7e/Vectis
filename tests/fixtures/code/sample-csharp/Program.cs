using System;
using SampleApp.Models;
using SampleApp.Services;

namespace SampleApp
{
    public class Program
    {
        public static void Main(string[] args)
        {
            var service = new UserService();
            var user = service.FindById("u-1");
            Console.WriteLine($"hello, {user.Name}");
        }
    }
}
